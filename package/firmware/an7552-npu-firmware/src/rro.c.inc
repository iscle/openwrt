/* SPDX-License-Identifier: MIT */
static volatile uint32_t *rx_words(uint32_t host)
{
	return (void *)(uintptr_t)((host & 0x3fffffff) | 0x40000000);
}

static void native_rx_refill(uint32_t band)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *data =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				    band * 64);
	volatile uint32_t *free_ids = rx_words(AN7552_RX_FREE_HOST);
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST);
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
		if (token >= AN7552_RX_BUFFER_COUNT || states[token]) {
			q[6] = 2;
			return;
		}
		q[20] = (q[20] + 1) % AN7552_RX_BUFFER_COUNT;
		q[22]--;
		states[token] = 1 | (band << 8);
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
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST),
			  *free_ids = rx_words(AN7552_RX_FREE_HOST);
	if (token >= AN7552_RX_BUFFER_COUNT || !states[token] ||
	    q[22] >= AN7552_RX_BUFFER_COUNT) {
		q[6] = 5;
		return;
	}
	states[token] = 0;
	free_ids[q[21]] = token;
	q[21] = (q[21] + 1) % AN7552_RX_BUFFER_COUNT;
	q[22]++;
}
static int native_rx_deliver(uint32_t token, uint32_t ppe_info)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *saved = rx_metadata(token), *data, *host,
			  *completion, *sidecar;
	volatile uint8_t *packet = (void *)rx_words(saved[0]), *destination;
	uint32_t band = (rx_words(AN7552_RX_STATE_HOST)[token] >> 8) & 1;
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
/* Per-batch copies of shared indexes and counters, published once per batch. */
struct rro_batch {
	uint32_t index, phase, mode, done, secure, chain;
	uint32_t ppe_producer, ppe_hw, ppe_pending;
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

static void native_rro_flush(struct rro_batch *b)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);

	native_ppe_flush(b);
	/* Validation can succeed even when this batch cannot deliver a frame. */
	if (b->secure) {
		q[23] += b->secure;
		b->secure = 0;
	}
	if (!b->done)
		return;
	barrier();
	*rx_words(AN7552_RX_EMI_HOST) =
		b->index ? b->index - 1 : AN7552_RX_RING_COUNT - 1;
	q[16] = b->index;
	q[17] = b->phase;
	q[30] = b->chain;
	q[7] += b->done;
	b->done = 0;
}

#define RRO_EMPTY 0
#define RRO_DONE 1
#define RRO_BLOCKED 2
static int native_rro_receive_one(struct rro_batch *b)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST), *descriptor,
			  *saved, *station;
	volatile uint8_t *packet;
	struct rx_metadata parsed;
	uint32_t d0, d1, d2, d3, state, length, address, token, reason,
		cookie = 0, security, band, offset = 0, fast = 0;

	/* rx_parse() fills these only on success; keep gcc -Werror quiet. */
	parsed.wcid = parsed.band = parsed.bss = 0;
	parsed.offset = parsed.length = 0;
	descriptor = rx_words(AN7552_RX_COMPLETE_HOST + b->index * 16);
	d3 = descriptor[3];
	if ((d3 >> 28) != b->phase)
		return RRO_EMPTY;
	barrier();
	d0 = descriptor[0];
	d1 = descriptor[1];
	d2 = descriptor[2];
	length = (d1 >> 16) & 16383;
	address = d0;
	token = (d2 >> 16) - AN7552_RX_TOKEN_BASE;
	reason = (d2 >> 12) & 15;
	state = token < AN7552_RX_BUFFER_COUNT ? states[token] : 0;
	if (token >= AN7552_RX_BUFFER_COUNT || (state & 255) != 1 ||
	    length > 2048 || address != AN7552_RX_BUFFER_HOST + token * 2048) {
		q[8] = length;
		q[9] = reason;
		q[15] = token;
		q[6] = 3;
		return RRO_EMPTY;
	}
	packet = (void *)rx_words(address);
	saved = rx_metadata(token);
	band = (state >> 8) & 1;
	if (!(state & 0x10000)) {
		saved[0] = d0;
		saved[1] = d1;
		saved[2] = d2;
		saved[3] = d3;
		/* A buffer after a non-final one continues the previous MPDU and
		 * starts with payload, not a receive descriptor: never parse it
		 * or let its bytes update replay counters.
		 */
		if (b->chain)
			reason = 1;
		security = (reason == 1 || reason == 2) ?
				   0 :
				   native_rx_security(packet, length, &cookie);
		saved[4] = security;
		saved[5] = cookie;
		b->secure += ((security & 15) == 1);
		if ((security & 15) == 2)
			q[24]++;
		uint32_t parsed_result =
			rx_parse(packet, length, reason, &parsed);
		if ((security & 15) == 1 && parsed_result != RX_PARSE_OK)
			(*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
							   0x780 +
							   parsed_result *
								   4))++;
		/* Classify using the currently authorized station identity.
		 * Only frames held entirely in this buffer can be forwarded.
		 */
		if (b->mode == 2 && (security & 15) == 1 &&
		    (d1 & (1U << 30)) &&
		    parsed_result == RX_PARSE_OK &&
		    parsed.wcid == (security >> 4) && parsed.band < 2) {
			station = (void *)(uintptr_t)(AN7552_STATION_NPU +
						      parsed.wcid * 32);
			volatile uint8_t *source = packet + parsed.offset + 6;
			if (station[0] && (station[1] & 255) == parsed.band &&
			    station[2] == packet_mac_low(source) &&
			    station[3] == packet_mac_high(source)) {
				offset = parsed.offset;
				fast = parsed.length;
			} else
				q[27]++;
		}
		saved[6] = offset;
		saved[7] = fast;
		states[token] = state | 0x10000;
	} else {
		offset = saved[6];
		fast = saved[7];
	}
	/* Linux can revoke station eligibility before the core 1 handshake. */
	if (fast) {
		uint32_t wcid = saved[4] >> 4;
		station = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
		if (!station[0] || !pn_entry(wcid)[0] ||
		    pn_entry(wcid)[1] != saved[5]) {
			fast = 0;
			saved[7] = 0;
		}
	}
	if (fast) {
		if (!native_ppe_queue(b, token, address + offset, fast))
			return RRO_BLOCKED;
		states[token] = 2 | (band << 8);
	} else {
		volatile uint32_t *ppe =
			(void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);

		/* Earlier PPE input may still need delivery to the same host ring.
		 * Publish outstanding work and let the bounded retry drain it.
		 */
		native_ppe_flush(b);
		if (ppe[12])
			return RRO_BLOCKED;
		if (!native_rx_deliver(token, 0))
			return RRO_BLOCKED;
	}
	/* Data1 bit 30 marks the last buffer of an MPDU. */
	b->chain = !(d1 & (1U << 30));
	b->index++;
	if (b->index == AN7552_RX_RING_COUNT) {
		b->index = 0;
		b->phase = (b->phase + 1) & 15;
	}
	b->done++;
	return RRO_DONE;
}

#define RRO_BATCH 32U
/* Refill and publish shared state once per batch, not once per frame. */
static void native_rro_receive(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *ppe =
		(void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	struct rro_batch b;
	int result;

	if (!q[4]) {
		q[5] = 0;
		return;
	}
	q[5] = 1;
	if (q[6])
		return;
	native_rx_refill(0);
	native_rx_refill(1);
	b.index = q[16];
	b.phase = q[17];
	b.chain = q[30];
	b.mode = ppe[0];
	b.done = 0;
	b.secure = 0;
	b.ppe_producer = ppe[10];
	b.ppe_hw = read32(0x1fb5080c);
	b.ppe_pending = 0;
	for (uint32_t n = 0; n < RRO_BATCH; n++) {
		result = native_rro_receive_one(&b);
		if (result == RRO_DONE)
			continue;
		if (result == RRO_EMPTY || q[6])
			break;
		/* PPE input/completions or host ring blocked: drain once and retry. */
		native_rro_flush(&b);
		native_ppe_service();
		b.ppe_hw = read32(0x1fb5080c);
		if (q[6] || native_rro_receive_one(&b) != RRO_DONE)
			break;
	}
	native_rro_flush(&b);
}

