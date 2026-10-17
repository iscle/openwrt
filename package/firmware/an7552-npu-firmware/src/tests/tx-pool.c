/* SPDX-License-Identifier: MIT */
/* Simulates TDMA, the core 0 download loop and the radio for the direct
 * transmit pool. Core 0 is the single owner of the TDMA ring and, with
 * forwarding, consumes it in native_forward_ring() without a work handoff.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static void barrier(void)
{
	__sync_synchronize();
}

static uint32_t read32(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

#include "../tdma.c.inc"
#include "../station.c.inc"
#include "../tx-pool.c.inc"
#include "../native-tx.c.inc"
#include "../txfree.c.inc"

#define NPU(x) ((volatile uint32_t *)(uintptr_t)(x))
#define FIFO 8192U

static void map(uint32_t address, size_t length)
{
	assert(mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)address);
}

static const uint8_t mac[6] = { 0x06, 0xcd, 0x1f, 0x73, 0xf3, 0xe6 };
static uint32_t hw_dma[2], radio_seen[2];
static uint32_t fifo[FIFO], fifo_head, fifo_tail;
static uint8_t radio_holds[TX_POOL_BUFFERS];
static uint32_t received[2], sent, dropped, bad_every;
static struct tx_band download[2];
/* Radio model knobs: fetch budget per step (0 = all), greedy TDMA that
 * ignores the CPU index, host frames produced per step.
 */
static uint32_t fetch_budget, greedy, host_frames, fetched_total[2];
static uint32_t host_token = AN7552_DIRECT_TOKENS;

static volatile uint32_t *ring(uint32_t band, uint32_t slot)
{
	return NPU(AN7552_TDMA_RING_NPU +
		   (band * AN7552_TDMA_COUNT + slot) * 32);
}

static uint32_t cpu_index(uint32_t band)
{
	return read32(0x1fb50900 + band * 16 + 8);
}

static uint32_t buffer_id(uint32_t host)
{
	assert(tdma_buffer_valid(host));
	return (host - AN7552_TDMA_BUFFER_HOST) / AN7552_TDMA_BUFFER_SIZE;
}

/* The frame engine fills armed slots up to, not including, the CPU index. */
static uint32_t tdma_fill(uint32_t band, uint32_t budget)
{
	uint32_t n = 0;

	/* greedy: a TDMA that ignores the CPU index and fills every slot
	 * whose descriptor is not marked done.
	 */
	while (n < budget &&
	       (greedy ? !(ring(band, hw_dma[band])[1] & 0x80000000) :
			 hw_dma[band] != cpu_index(band))) {
		volatile uint32_t *d = ring(band, hw_dma[band]);
		uint32_t id = buffer_id(d[2]), bad;
		volatile uint8_t *payload = (void *)(uintptr_t)TX_BUFFER_NPU(id);

		assert(!(d[1] & 0x80000000));
		/* The radio must never still own a buffer the TDMA writes,
		 * fetched or still queued on the radio ring.
		 */
		assert(!radio_holds[id]);
		if (fetch_budget)
			for (uint32_t t = 0; t < TX_POOL_TOKENS; t++)
				assert(tx_pool()->token_buffer[t] !=
				       (TX_TOKEN_VALID | (band ? TX_TOKEN_BAND : 0) | id));
		bad = bad_every && !(received[band] % bad_every);
		for (uint32_t i = 0; i < 6; i++)
			payload[i] = mac[i] ^ (bad && i == 5);
		d[4] = (1U << 14) | (band << 25);
		d[6] = 0;
		d[1] = 0x80000000 | 1514;
		received[band]++;
		/* Band 0 frames never match the band 1 station either. */
		if (bad && band)
			dropped++;
		hw_dma[band] = (hw_dma[band] + 1) % AN7552_TDMA_COUNT;
		n++;
	}
	return n;
}

static void radio_fetch(uint32_t band)
{
	volatile uint32_t *q =
		NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + band * 64);
	uint32_t regs = band ? 0x200d8450 : 0x200d4420, n = 0;

	/* The producers never overran the radio: at most count - 1 pending. */
	assert(q[8] + q[12] - fetched_total[band] < AN7552_NATIVE_RING_COUNT);
	while (radio_seen[band] != q[4] && (!fetch_budget || n < fetch_budget)) {
		volatile uint32_t *d =
			NPU(0x44240000 + band * 0x8000 + radio_seen[band] * 16);
		uint32_t token = (d[0] - AN7552_TXWI_HOST) / 128, id;
		volatile uint32_t *txwi = NPU(AN7552_TXWI_NPU + token * 128);

		n++;
		fetched_total[band]++;
		radio_seen[band] = (radio_seen[band] + 1) %
				   AN7552_NATIVE_RING_COUNT;
		if (token >= AN7552_DIRECT_TOKENS) {
			/* Host frame from native_transmit(): Linux owns it. */
			assert(d[2] == 0x84320000U && txwi[8] >> 16 == token);
			continue;
		}
		assert(txwi[8] >> 16 == token && txwi[10] == d[2]);
		assert(txwi[16] == 1514 && (txwi[9] >> 24) == 1);
		for (uint32_t i = 0; i < 19; i++)
			assert(!txwi[i] || i == 8 || i == 9 || i == 10 ||
			       i == 16);
		id = buffer_id(d[2]);
		assert(!radio_holds[id]);
		radio_holds[id] = 1;
		fifo[fifo_tail++ % FIFO] = token;
		assert(fifo_tail - fifo_head <= FIFO);
		sent++;
	}
	*NPU(regs + 12) = radio_seen[band];
}

/* Linux queues one frame on the host ring of band 1 for native_transmit(). */
static void host_queue(void)
{
	volatile uint32_t *q = NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + 64);
	volatile uint32_t *host = NPU(0x44300000U + q[2] * 208);

	if ((q[2] + 1) % q[1] == q[3])
		return;
	for (uint32_t i = 0; i < 52; i++)
		host[i] = 0;
	host[0] = 1 | (100U << 1);
	host[1] = 0x84320000U;
	host[4 + 8] = host_token << 16;
	host_token = host_token + 1 < AN7552_DIRECT_TOKENS + 64 ?
			     host_token + 1 :
			     AN7552_DIRECT_TOKENS;
	q[2] = (q[2] + 1) % q[1];
}

static void radio_complete(uint32_t count)
{
	uint32_t event[2 + 64], txq[16] = { 0 }, n = 0, length;

	while (count && fifo_head != fifo_tail) {
		uint32_t token = fifo[fifo_head++ % FIFO];
		uint32_t id = tx_pool()->token_buffer[token] & TX_TOKEN_BUFFER;

		assert(radio_holds[id]);
		radio_holds[id] = 0;
		event[2 + n / 2] = n & 1 ? event[2 + n / 2] | (token << 15) :
					   token;
		n++;
		count--;
		if (n == 128 || !count || fifo_head == fifo_tail) {
			if (n & 1)
				event[2 + n / 2] |= 32767U << 15;
			event[0] = (6U << 27) | (n << 16);
			event[1] = 5U << 16;
			length = 8 + (n + 1) / 2 * 4;
			assert(!txfree_walk((void *)event, length, txq, 0));
			assert(!txfree_walk((void *)event, length, txq, 1));
			/* native_txfree publishes after each committed event. */
			tx_pool_publish(tx_pool());
			n = 0;
		}
	}
}

static uint32_t published_consumer(uint32_t band)
{
	return read32(AN7552_NPU_BASE + AN7552_TDMA_CONTROL + 4 + band * 4);
}

/* Snapshot of the TDMA descriptors the NPU must not write during a step:
 * slots owned by the TDMA hardware (armed, not yet filled) and filled slots
 * the consumer has not reached.
 */
static uint32_t snap[2][AN7552_TDMA_COUNT][2], owned[2][AN7552_TDMA_COUNT];

static void snapshot(void)
{
	for (uint32_t band = 0; band < 2; band++) {
		uint32_t s = download[band].consumer;
		uint32_t end = (cpu_index(band) + 1) % AN7552_TDMA_COUNT;

		memset(owned[band], 0, sizeof(owned[band]));
		if (!download[band].active)
			continue;
		/* From the consumer up to and including the CPU index slot. */
		for (; s != end; s = (s + 1) % AN7552_TDMA_COUNT) {
			owned[band][s] = 1;
			snap[band][s][0] = ring(band, s)[1];
			snap[band][s][1] = ring(band, s)[2];
		}
	}
}

static void check_untouched(uint32_t band, uint32_t consumed_from)
{
	for (uint32_t s = 0; s < AN7552_TDMA_COUNT; s++) {
		uint32_t k = (s + AN7552_TDMA_COUNT - consumed_from) %
			     AN7552_TDMA_COUNT;
		uint32_t n = (download[band].consumer + AN7552_TDMA_COUNT -
			      consumed_from) % AN7552_TDMA_COUNT;

		/* Slots consumed in this step may be re-armed; no others. */
		if (!owned[band][s] || k < n)
			continue;
		assert(ring(band, s)[1] == snap[band][s][0]);
		assert(ring(band, s)[2] == snap[band][s][1]);
	}
}

/* One pass of the core 0 loop: core 0 is the only owner of the TDMA ring
 * (consume, forward, consumer publish and re-arm) and keeps the TDMA status
 * words in the core 1 state block that Linux reads.
 */
static void step(void)
{
	volatile uint32_t *core1 = NPU(AN7552_NPU_BASE + AN7552_STRIDE);

	for (uint32_t i = 0; i < host_frames; i++)
		host_queue();
	native_transmit(0);
	native_transmit(1);
	snapshot();
	for (uint32_t band = 0; band < 2; band++) {
		uint32_t from = download[band].consumer;

		native_download(core1, &download[band], band);
		check_untouched(band, from);
		assert(published_consumer(band) == download[band].consumer);
		radio_fetch(band);
	}
}

static uint32_t armed(uint32_t band)
{
	return (cpu_index(band) + AN7552_TDMA_COUNT - hw_dma[band]) %
	       AN7552_TDMA_COUNT;
}

/* Every buffer is exactly in one place: pool, ring slot or radio. */
static void mark(uint8_t *seen, uint32_t id)
{
	assert(id < TX_POOL_BUFFERS && !seen[id]);
	seen[id] = 1;
}

static void check_conservation(void)
{
	static uint8_t seen[TX_POOL_BUFFERS];
	struct tx_pool *p = tx_pool();
	uint32_t total = 0;

	memset(seen, 0, sizeof(seen));
	for (uint32_t i = 0; i < p->free_buffers; i++, total++)
		mark(seen, p->buffer[i]);
	for (uint32_t band = 0; band < 2; band++) {
		/* Slots from the refill pointer up to the consumer own nothing. */
		uint32_t empty = (download[band].consumer + AN7552_TDMA_COUNT -
				  p->arm[band]) % AN7552_TDMA_COUNT;

		for (uint32_t k = empty; k < AN7552_TDMA_COUNT; k++, total++) {
			uint32_t s = (p->arm[band] + k) % AN7552_TDMA_COUNT;
			uint32_t id = p->slot[band][s];

			assert(ring(band, s)[2] == TX_BUFFER_HOST(id));
			mark(seen, id);
		}
	}
	for (uint32_t t = 0; t < TX_POOL_TOKENS; t++) {
		uint32_t v = p->token_buffer[t];

		if (!(v & TX_TOKEN_VALID))
			continue;
		/* A slow radio has not fetched every queued frame yet. */
		assert(radio_holds[v & TX_TOKEN_BUFFER] || fetch_budget);
		mark(seen, v & TX_TOKEN_BUFFER);
		total++;
	}
	assert(total == TX_POOL_BUFFERS);
}

int main(void)
{
	volatile uint32_t *forward = NPU(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *station = NPU(AN7552_STATION_NPU + 32);
	struct tx_pool *p;
	uint32_t start[2] = { 100, 1000 }, high = 0;

	map(AN7552_NPU_BASE, 4096);
	map(0x44240000, 0x10000);
	map(0x44280000, 0x30000);
	map(0x44400000, 0x640000);
	map(TX_POOL_ADDR, 0xc000);
	map(0x200d4000, 0x5000);
	map(0x1fb50000, 4096);
	map(0x44300000, 0x20000);
	map(0x44a40000, 0x10000);
	(void)native_txfree;

	/* Linux: identity buffers in every slot, CPU index one behind. */
	for (uint32_t band = 0; band < 2; band++) {
		for (uint32_t i = 0; i < AN7552_TDMA_COUNT; i++) {
			ring(band, i)[1] = AN7552_TDMA_BUFFER_SIZE;
			ring(band, i)[2] = TX_BUFFER_HOST(band * AN7552_TDMA_COUNT + i);
		}
		*NPU(0x1fb50900 + band * 16 + 12) = start[band];
		*NPU(0x1fb50900 + band * 16 + 8) =
			(start[band] + AN7552_TDMA_COUNT - 1) % AN7552_TDMA_COUNT;
		hw_dma[band] = start[band];
		NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + band * 64)[5] = 1;
	}
	/* Band 1 host ring for native_transmit(), empty for now. */
	NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + 64)[0] = 0x84300000U;
	NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + 64)[1] = 64;
	station[0] = 1;
	station[1] = 1;
	station[2] = mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24;
	station[3] = mac[4] | mac[5] << 8;
	station[4] = 0x80;
	forward[1] = 2;
	forward[3] = 1;
	forward[4] = 0x80;
	forward[6] = start[0];
	forward[7] = start[1];
	forward[0] = 1;
	*NPU(AN7552_NPU_BASE + AN7552_TDMA_CONTROL) = 1;
	/* stale TXWI contents from an earlier firmware */
	memset((void *)(uintptr_t)AN7552_TXWI_NPU, 0xa5,
	       AN7552_DIRECT_TOKENS * 128);
	tx_pool_boot();
	step();
	p = tx_pool();
	assert(p->ready == 3 && p->free_buffers == 992);

	/* 1. A radio that holds everything: band 1 queues ~2x the ring and the
	 * pool runs dry, so refill stops. The consumer must stop one slot
	 * before the first unrefilled slot, also with a TDMA that ignores the
	 * CPU index and fills that last armed slot (second round).
	 */
	for (greedy = 0; greedy < 2; greedy++) {
		for (uint32_t i = 0; i < 400; i++) {
			tdma_fill(1, 64);
			step();
		}
		assert(p->free_buffers == 0);
		assert(p->inflight[1] >= 2000 && p->inflight[1] <= 2016);
		assert((download[1].consumer + 1) % AN7552_TDMA_COUNT ==
		       p->arm[1]);
		assert(hw_dma[1] == (greedy ? p->arm[1] : cpu_index(1)));
		printf("held radio%s: band1 in flight %u (ring %u), armed %u\n",
		       greedy ? " (greedy TDMA)" : "", p->inflight[1],
		       AN7552_TDMA_COUNT, armed(1));
		check_conservation();
		/* The greedy TDMA's frame in the last armed slot goes out once
		 * refill moves on, so complete twice.
		 */
		for (uint32_t i = 0; i < 4; i++) {
			radio_complete(FIFO);
			step();
		}
		radio_complete(FIFO);
		assert(p->inflight[1] == 0 && p->free_tokens == TX_POOL_TOKENS);
		assert(forward[8] == forward[9]);
		assert(armed(1) == AN7552_TDMA_COUNT - 1);
		assert(published_consumer(1) == hw_dma[1]);
		check_conservation();
	}
	greedy = 0;

	/* 2. Steady state with radio latency, drops and both bands. */
	bad_every = 97;
	for (uint32_t i = 0; i < 20000; i++) {
		tdma_fill(1, 40);
		if (!(i % 3))
			tdma_fill(0, 8);
		step();
		if (fifo_tail - fifo_head > 1800)
			radio_complete(30);
		if (p->inflight[0] + p->inflight[1] > high)
			high = p->inflight[0] + p->inflight[1];
		if (!(i % 1000))
			check_conservation();
		assert(p->inflight[0] <= AN7552_TDMA_COUNT);
	}
	printf("steady: high %u, drops %u, band0 armed %u\n", high,
	       forward[12], armed(0));
	assert(high > 1800);
	/* Idle 2.4 GHz ring hands buffers back down to its cap. */
	assert(armed(0) <= 512);
	for (uint32_t i = 0; i < 200; i++) {
		step();
		radio_complete(FIFO);
	}
	assert(forward[8] == received[0] + received[1]);
	assert(forward[12] == dropped + received[0]);
	assert(forward[8] == forward[9] && forward[10] == sent);
	assert(p->free_tokens == TX_POOL_TOKENS && !p->errors);
	assert(forward[13] == 0 && (forward[5] & 0xffff) >= high);
	/* Idle: every filled slot was consumed and published to Linux. */
	for (uint32_t band = 0; band < 2; band++)
		assert(published_consumer(band) == hw_dma[band]);
	assert(NPU(AN7552_NPU_BASE + AN7552_STRIDE)[12] == 3);
	check_conservation();

	/* 2b. Host frames share the radio ring with a slow radio, and a TDMA
	 * that ignores the CPU index: the cached radio index
	 * must never let a producer overrun the radio (checked in radio_fetch)
	 * and the consumer must never take a slot that was not re-armed
	 * (tdma_fill and check_conservation catch a double-used buffer).
	 */
	{
		volatile uint32_t *q1 =
			NPU(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + 64);
		uint32_t pending, most = 0, base8 = q1[8];

		greedy = 1;
		fetch_budget = 3;
		host_frames = 1;
		bad_every = 0;
		for (uint32_t i = 0; i < 20000; i++) {
			tdma_fill(1, 8);
			step();
			radio_complete(FIFO);
			pending = q1[8] + q1[12] - fetched_total[1];
			if (pending > most)
				most = pending;
			if (!(i % 1000))
				check_conservation();
		}
		printf("shared ring: most pending %u of %u, host frames %u\n",
		       most, AN7552_NATIVE_RING_COUNT, q1[8] - base8);
		/* Measured after the radio fetched 3: the ring was full. */
		assert(most == AN7552_NATIVE_RING_COUNT - 1 - 3);
		assert(q1[8] - base8 > 1000 && !q1[7]);
		fetch_budget = 0;
		host_frames = 0;
		for (uint32_t i = 0; i < 200; i++) {
			step();
			radio_complete(FIFO);
		}
		assert(forward[8] == forward[9] && forward[10] == sent);
		assert(p->free_tokens == TX_POOL_TOKENS && !p->errors);
		for (uint32_t band = 0; band < 2; band++)
			assert(published_consumer(band) == hw_dma[band]);
		check_conservation();
		greedy = 0;
	}

	/* 3. Teardown and restart the way Linux does it (tdma_stop, then
	 * tdma_start): stop forwarding, disable TDMA, re-arm every slot with
	 * its identity buffer, set up forwarding from the TDMA index, enable.
	 */
	{
		volatile uint32_t *core1 = NPU(AN7552_NPU_BASE + AN7552_STRIDE);

		forward[0] = 0;
		step();
		assert(!p->ready && !(forward[14] & 3));
		*NPU(AN7552_NPU_BASE + AN7552_TDMA_CONTROL) = 0;
		step();
		assert(!(core1[12] & 3));
		for (uint32_t band = 0; band < 2; band++) {
			for (uint32_t i = 0; i < AN7552_TDMA_COUNT; i++) {
				ring(band, i)[1] = AN7552_TDMA_BUFFER_SIZE;
				ring(band, i)[2] = TX_BUFFER_HOST(
					band * AN7552_TDMA_COUNT + i);
			}
			*NPU(0x1fb50900 + band * 16 + 12) = hw_dma[band];
		}
		for (uint32_t i = 0; i < 16; i++)
			forward[i] = 0;
		forward[1] = 2;
		forward[3] = 1;
		forward[4] = 0x80;
		forward[6] = hw_dma[0];
		forward[7] = hw_dma[1];
		forward[0] = 1;
		*NPU(AN7552_NPU_BASE + AN7552_TDMA_CONTROL) = 1;
		step();
		assert((core1[12] & 3) == 3 && (forward[14] & 3) == 3);
		assert(p->ready == 3 && p->free_buffers == 992);
		for (uint32_t band = 0; band < 2; band++) {
			assert(published_consumer(band) == hw_dma[band]);
			*NPU(0x1fb50900 + band * 16 + 8) =
				(hw_dma[band] + AN7552_TDMA_COUNT - 1) %
				AN7552_TDMA_COUNT;
		}
		check_conservation();
		bad_every = 13;
		for (uint32_t i = 0; i < 2000; i++) {
			tdma_fill(1, 40);
			tdma_fill(0, 4);
			step();
			radio_complete(64);
		}
		for (uint32_t i = 0; i < 100; i++) {
			step();
			radio_complete(FIFO);
		}
		assert(forward[8] && forward[8] == forward[9] && !forward[13]);
		for (uint32_t band = 0; band < 2; band++)
			assert(published_consumer(band) == hw_dma[band]);
		assert(p->free_tokens == TX_POOL_TOKENS && !p->errors);
		check_conservation();
	}
	puts("PASS: TX pool ownership, refill order, caps, drops, completion and restart");
	puts("PASS: fused TDMA consume: no writes to TDMA-owned slots, shared radio ring, greedy TDMA");
	return 0;
}
