/* SPDX-License-Identifier: MIT */
static volatile uint32_t *rx_words(uint32_t host)
{
	return (void *)(uintptr_t)((host & 0x3fffffff) | 0x40000000);
}

#ifndef RX_HEADER_ALIAS
/* DMA writes are not coherent with the NPU data cache. Cache only the
 * per-packet stack copy in pkt_view, never the reusable DMA buffer.
 */
#define RX_HEADER_ALIAS(host) ((volatile uint8_t *)rx_words(host))
#endif

#ifndef RX_STATE_MIRROR
#define RX_STATE_MIRROR 1
#endif
/* Core 1 owns the receive buffer states after setup. Keep them as one byte
 * per buffer in local SRAM (~14 cycles) instead of words in DDR (~80).
 * Byte layout: bits 0-1 state, bit 2 band, bit 3 parsed.
 */
#define RX_STATE_SRAM 0x3e900000U
static inline uint32_t st_get(uint32_t token)
{
#if RX_STATE_MIRROR
	uint32_t b = ((volatile uint8_t *)(uintptr_t)RX_STATE_SRAM)[token];

	return (b & 3) | ((b & 4) << 6) | ((b & 8) << 13);
#else
	return rx_words(AN7552_RX_STATE_HOST)[token];
#endif
}

static inline void st_set(uint32_t token, uint32_t value)
{
#if RX_STATE_MIRROR
	((volatile uint8_t *)(uintptr_t)RX_STATE_SRAM)[token] =
		(value & 3) | ((value >> 6) & 4) | ((value >> 13) & 8);
#else
	rx_words(AN7552_RX_STATE_HOST)[token] = value;
#endif
}

/* Last PPE hardware consumer index read by core 1 and the PPE producer it
 * was read against ([0], [1]), in local SRAM right after the buffer states.
 * The PPE only advances its index, so while nobody else moved the producer
 * a stale copy can only make the 64-entry ring look fuller;
 * native_ppe_queue() re-reads it when the ring looks full.
 */
#define RX_PPE_CACHE                                                        \
	((volatile uint32_t *)(uintptr_t)(RX_STATE_SRAM +                   \
					  AN7552_RX_BUFFER_COUNT))
#define RX_PPE_STALE 0xffffffffU

/* Linux initializes the DDR copy before enabling receive processing. */
static void st_load(void)
{
	RX_PPE_CACHE[1] = RX_PPE_STALE;
#if RX_STATE_MIRROR
	volatile uint32_t *ddr = rx_words(AN7552_RX_STATE_HOST);

	for (uint32_t t = 0; t < AN7552_RX_BUFFER_COUNT; t++)
		st_set(t, ddr[t]);
#endif
}

static void native_rx_refill(uint32_t band)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *data =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				    band * 64);
	volatile uint32_t *free_ids = rx_words(AN7552_RX_FREE_HOST);
	uint32_t producer = data[6], consumer = read32(data[4] + 12), filled = 0;

	data[8] = consumer;
	if (consumer >= AN7552_RX_RING_COUNT) {
		q[6] = 1;
		return;
	}
	for (uint32_t budget = 0; budget < 32; budget++) {
		volatile uint32_t *descriptor;
		uint32_t token, next = (producer + 1) % AN7552_RX_RING_COUNT;

		if (next == consumer || !q[22])
			break;
		token = free_ids[q[20]];
		if (token >= AN7552_RX_BUFFER_COUNT || st_get(token)) {
			q[6] = 2;
			return;
		}
		q[20] = (q[20] + 1) % AN7552_RX_BUFFER_COUNT;
		q[22]--;
		st_set(token, 1 | (band << 8));
		descriptor = rx_words(AN7552_RX_DATA_RING_HOST + band * 0x8000 +
				      producer * 16);
		descriptor[0] = AN7552_RX_BUFFER_HOST + token * 2048;
		descriptor[2] = (AN7552_RX_TOKEN_BASE + token) << 16;
		descriptor[1] = (data[11] << 16) | (1U << 8);
		barrier();
		descriptor[3] = data[9] << 28;
		if (!next)
			data[9] = (data[9] + 1) & 15;
		filled = producer;
		producer = next;
	}
	if (producer != data[6]) {
		barrier();
		*(volatile uint32_t *)(uintptr_t)(data[4] + 8) = filled;
	}
	data[6] = producer;
}

static void native_ppe_service(void);
static volatile uint32_t *rx_metadata(uint32_t token)
{
	return rx_words(0x841b0000 + token * 32);
}
static void native_rx_release(uint32_t token)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *free_ids = rx_words(AN7552_RX_FREE_HOST);
	if (token >= AN7552_RX_BUFFER_COUNT || !st_get(token) ||
	    q[22] >= AN7552_RX_BUFFER_COUNT) {
		q[6] = 5;
		return;
	}
	st_set(token, 0);
	free_ids[q[21]] = token;
	q[21] = (q[21] + 1) % AN7552_RX_BUFFER_COUNT;
	q[22]++;
}
/* Copy one receive buffer to the Linux ring. saved[1..5] are the receive
 * descriptor words 1..3, the security verdict and the replay cookie, read
 * either from the DDR metadata of the token (frames returned by the PPE) or
 * from an upload pipeline record in SRAM (upsplit.c.inc). The packet address
 * is the token's buffer: every receive path checks that the descriptor
 * address equals it before anything is saved or queued.
 */
static int rx_deliver_words(uint32_t token, uint32_t band,
			    volatile uint32_t *saved, uint32_t ppe_info)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *data, *host, *completion, *sidecar;
	volatile uint8_t *packet = (void *)rx_words(AN7552_RX_BUFFER_HOST +
						    token * 2048),
			 *destination;
	uint32_t length = (saved[1] >> 16) & 16383, producer = q[2], host_index,
		 info, copied = 0;
	data = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				   band * 64);
	host_index = data[2];
	if (producer == (*rx_words(q[18]) & 65535) || host_index == data[5])
		return 0;
	host = rx_words(data[0] + host_index * 16);
	info = host[1];
	if ((info & (1U << 31)) || ((info >> 16) & 16383) < length ||
	    host[0] < 0x80000000 || host[0] > 0xa0000000 - length) {
		q[6] = 4;
		return 0;
	}
	destination = (void *)rx_words(host[0]);
	sidecar = rx_words(0x842f9000 + producer * 16);
	sidecar[0] = saved[4];
	sidecar[1] = saved[5];
	sidecar[2] = ppe_info;
	if (!(((uintptr_t)packet | (uintptr_t)destination) & 3))
		for (; copied + 4 <= length; copied += 4)
			*(volatile uint32_t *)(destination + copied) =
				*(volatile uint32_t *)(packet + copied);
	for (; copied < length; copied++)
		destination[copied] = packet[copied];
	completion = rx_words(q[0] + producer * 16);
	completion[0] = host[0];
	completion[1] = saved[1];
	completion[2] = (saved[2] & 65535) | (host[2] & 0xffff0000);
	barrier();
	host[1] = (info & 65535) | (length << 16) | (1U << 31) | (1U << 30);
	data[2] = (host_index + 1) % data[1];
	data[7]++;
	barrier();
	completion[3] = (saved[3] & 0x0fffffff) | (q[3] << 28);
	producer++;
	if (producer == q[1]) {
		producer = 0;
		q[3] = (q[3] + 1) & 15;
	}
	barrier();
	q[2] = producer;
	*(volatile uint32_t *)(uintptr_t)0x1ec0d1a8 = producer;
	native_rx_release(token);
	return 1;
}

static int native_rx_deliver(uint32_t token, uint32_t ppe_info)
{
	return rx_deliver_words(token, (st_get(token) >> 8) & 1,
				rx_metadata(token), ppe_info);
}
/* Per-call copies of the PPE input state and counters, published once per
 * call by native_ppe_flush().
 */
struct rro_batch {
	uint32_t mode, done;
	uint32_t ppe_producer, ppe_hw, ppe_pending;
	/* Station identity words 1..3 of st_wcid. Linux rewrites them only
	 * under the quiesce handshake, which never overlaps a call. Word 0
	 * (eligibility) can be revoked at any time and is always read live.
	 */
	uint32_t st_wcid, st1, st2, st3;
};

static int native_ppe_queue(struct rro_batch *b, uint32_t token,
			    uint32_t address, uint32_t length)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	uint32_t producer = b->ppe_producer, next = (producer + 1) % 64;
	volatile uint32_t *descriptor = rx_words(0x842f0000 + producer * 32);

	if (b->mode != 2 || q[3])
		return 0;
	/* A stale hardware index only makes the ring look fuller. */
	if (next == b->ppe_hw) {
		b->ppe_hw = read32(0x1fb5080c);
		if (next == b->ppe_hw)
			return 0;
	}
	if (!(descriptor[1] & 0x80000000))
		return 0;
	descriptor[2] = address;
	descriptor[4] = 0x80000000 | (token << 14);
	descriptor[5] = 0x7f4087ff;
	barrier();
	descriptor[1] = length;
	barrier();
	/* Ring per frame so the PPE forwards while the next frame is parsed. */
	*(volatile uint32_t *)(uintptr_t)0x1fb50808 = next;
	b->ppe_producer = next;
	b->ppe_pending++;
	return 1;
}

static void native_ppe_flush(struct rro_batch *b)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);

	if (!b->ppe_pending)
		return;
	q[10] = b->ppe_producer;
	q[1] += b->ppe_pending;
	q[12] += b->ppe_pending;
	b->ppe_pending = 0;
}

static inline void rro_ppe_hw_load(struct rro_batch *b)
{
	volatile uint32_t *cache = RX_PPE_CACHE;

	b->ppe_hw = cache[1] == b->ppe_producer ? cache[0] :
						  read32(0x1fb5080c);
}

static inline void rro_ppe_hw_save(struct rro_batch *b)
{
	volatile uint32_t *cache = RX_PPE_CACHE;

	cache[0] = b->ppe_hw;
	cache[1] = b->ppe_producer;
}

/* Records core 1 takes per call (upsplit.c.inc). */
#define RRO_BATCH 32U
