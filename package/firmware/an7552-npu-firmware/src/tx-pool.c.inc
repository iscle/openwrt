/* SPDX-License-Identifier: MIT */
/* Direct transmit pool, owned by core 0.
 *
 * Ring slots, packet buffers and radio tokens are independent: a received
 * slot is refilled with a free buffer as soon as its frame is queued to the
 * radio, and the frame keeps its buffer and token until TXFREE. The number of
 * frames the radio may hold is then bounded by the token caps below instead
 * of by the 1024-slot TDMA ring of each band.
 *
 * The state lives in cached DDR between the stacks and the replay table; no
 * other agent reads it.
 */
#define TX_POOL_ADDR 0x84104000U
#define TX_POOL_TOKENS AN7552_DIRECT_TOKENS
#define TX_TOKEN_VALID 0x8000U
#define TX_TOKEN_BAND 0x4000U
#define TX_TOKEN_BUFFER 0x0fffU
/* Frames queued in the radio per band. Band 0 keeps the previous limit. */
#define TX_INFLIGHT_CAP(band) ((band) ? TX_POOL_TOKENS : AN7552_TDMA_COUNT)
/* Ring slots kept armed per band; an idle 2.4 GHz ring gives buffers back. */
#define TX_ARM_CAP(band) ((band) ? AN7552_TDMA_COUNT : 512U)
/* Never a ring index: forces a fresh radio consumer read. */
#define TX_RADIO_STALE 0xffffffffU

struct tx_pool {
	uint32_t ready;
	uint32_t free_buffers, free_tokens;
	uint32_t inflight[2], arm[2];
	uint32_t errors, high, completed;
	/* Last radio consumer index read over PCIe, valid while the native
	 * ring producer still equals radio_producer (see native_forward()).
	 */
	uint32_t radio_consumer[2], radio_producer[2];
	uint16_t buffer[TX_POOL_BUFFERS];
	uint16_t token[TX_POOL_TOKENS];
	uint16_t token_buffer[TX_POOL_TOKENS];
	uint16_t slot[AN7552_TDMA_BANDS][AN7552_TDMA_COUNT];
};

_Static_assert(sizeof(struct tx_pool) <= 0xc000, "tx pool overlaps PN table");
_Static_assert(AN7552_TDMA_BUFFER_HOST +
		       TX_POOL_BUFFERS * AN7552_TDMA_BUFFER_SIZE <=
	       AN7552_TXWI_HOST, "tx buffers overlap TXWI");

static inline struct tx_pool *tx_pool(void)
{
	return (struct tx_pool *)(uintptr_t)TX_POOL_ADDR;
}

static volatile uint32_t *tx_forward_control(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
}

static void tx_pool_publish(struct tx_pool *p)
{
	volatile uint32_t *forward = tx_forward_control();
	uint32_t total = p->inflight[0] + p->inflight[1];

	if (total > p->high)
		p->high = total;
	/* Completed == received means Linux may tear the workspace down. */
	forward[9] = p->completed;
	/* forward[5]: errors << 16 | high-water; forward[13]: band1 << 16 | band0. */
	forward[5] = (p->errors << 16) | (p->high & 0xffff);
	forward[13] = (p->inflight[1] << 16) | (p->inflight[0] & 0xffff);
}

static void tx_pool_boot(void)
{
	tx_pool()->ready = 0;
}

/* Linux armed every slot of the band with its identity buffer at setup. */
static void tx_pool_start(uint32_t band, uint32_t start)
{
	struct tx_pool *p = tx_pool();

	if (!(p->ready & 3)) {
		p->free_buffers = 0;
		for (uint32_t i = AN7552_DIRECT_TOKENS; i < TX_POOL_BUFFERS; i++)
			p->buffer[p->free_buffers++] = i;
		p->free_tokens = 0;
		for (uint32_t i = TX_POOL_TOKENS; i--;) {
			p->token[p->free_tokens++] = i;
			p->token_buffer[i] = 0;
		}
		p->inflight[0] = p->inflight[1] = 0;
		p->errors = 0;
		p->high = 0;
		p->completed = 0;
	}
	for (uint32_t i = 0; i < AN7552_TDMA_COUNT; i++)
		p->slot[band][i] = band * AN7552_TDMA_COUNT + i;
	p->arm[band] = start;
	p->radio_producer[band] = TX_RADIO_STALE;
	p->ready |= 1U << band;
}

static void tx_pool_stop(uint32_t band)
{
	tx_pool()->ready &= ~(1U << band);
}

/* Published to forward[9] by tx_pool_publish(). */
static void tx_pool_release_buffer(struct tx_pool *p, uint32_t buffer)
{
	p->completed++;
	if (p->free_buffers >= TX_POOL_BUFFERS) {
		p->errors++;
		return;
	}
	p->buffer[p->free_buffers++] = buffer;
}

static int tx_pool_token_busy(uint32_t token)
{
	return token < TX_POOL_TOKENS &&
	       (tx_pool()->token_buffer[token] & TX_TOKEN_VALID);
}

/* TXFREE for a direct token: return its buffer and token. */
static int tx_pool_complete(uint32_t token)
{
	struct tx_pool *p = tx_pool();
	uint32_t value, band;

	if (!tx_pool_token_busy(token) || p->free_tokens >= TX_POOL_TOKENS)
		return 1;
	value = p->token_buffer[token];
	band = !!(value & TX_TOKEN_BAND);
	p->token_buffer[token] = 0;
	p->token[p->free_tokens++] = token;
	if (p->inflight[band])
		p->inflight[band]--;
	tx_pool_release_buffer(p, value & TX_TOKEN_BUFFER);
	return 0;
}

/* Refill consumed slots in ring order, then advance the CPU index once.
 * Slots [arm, consumer) were consumed by native_forward() and hold no buffer
 * the TDMA may write; every other slot is armed. native_forward() never lets
 * the consumer reach arm from behind, so arm == consumer means "all armed".
 */
static void tx_pool_arm(uint32_t band, uint32_t consumer)
{
	struct tx_pool *p = tx_pool();
	uint32_t registers = 0x1fb50900 + band * 16, last = 0, armed = 0;
	uint32_t slot = p->arm[band];
	uint32_t unarmed = (consumer + AN7552_TDMA_COUNT - slot) %
			   AN7552_TDMA_COUNT;

	while (unarmed && p->free_buffers &&
	       AN7552_TDMA_COUNT - unarmed < TX_ARM_CAP(band)) {
		uint32_t buffer = p->buffer[--p->free_buffers];
		volatile uint32_t *descriptor =
			(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
					    (band * AN7552_TDMA_COUNT + slot) *
						    32);

		descriptor[2] = TX_BUFFER_HOST(buffer);
		descriptor[1] = AN7552_TDMA_BUFFER_SIZE;
		p->slot[band][slot] = buffer;
		last = slot;
		armed = 1;
		unarmed--;
		slot = (slot + 1) % AN7552_TDMA_COUNT;
	}
	if (armed) {
		p->arm[band] = slot;
		/* Descriptors are clean before the TDMA may use the slots. */
		barrier();
		*(volatile uint32_t *)(uintptr_t)(registers + 8) = last;
	}
}
