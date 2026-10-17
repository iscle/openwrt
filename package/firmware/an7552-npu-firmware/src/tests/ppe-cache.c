/* SPDX-License-Identifier: MIT */
/* The PPE hardware consumer index is kept across receive batches and read
 * again only when the 64-entry PPE ring looks full or the producer was moved
 * by someone else. The producer must never overtake the hardware.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static uint32_t hw, hw_reads;

static void barrier(void)
{
	__sync_synchronize();
}

static uint32_t read32(uint32_t address)
{
	if (address == 0x1fb5080c) {
		hw_reads++;
		return hw;
	}
	return *(volatile uint32_t *)(uintptr_t)address;
}

#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"
#define RX_STATE_MIRROR 0
#define RX_HEADER_ALIAS(host) ((volatile uint8_t *)rx_words(host))
#include "../rro.c.inc"
#include "../ppe.c.inc"
#include "../upsplit.c.inc"

static void map(uint32_t address, size_t length)
{
	assert(mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)address);
}

static volatile uint32_t *ppe(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
}

static volatile uint32_t *slot(uint32_t i)
{
	return rx_words(0x842f0000 + i * 32);
}

/* The PPE takes up to n queued descriptors and marks their slots done. */
static uint32_t hw_consume(uint32_t n)
{
	uint32_t producer = *(volatile uint32_t *)(uintptr_t)0x1fb50808, taken;

	for (taken = 0; taken < n && hw != producer; taken++) {
		slot(hw)[1] = 0x80000000;
		hw = (hw + 1) % 64;
	}
	return taken;
}

/* One core 1 call as native_up_back() runs it: load, queue, save. */
static uint32_t batch(uint32_t frames, uint32_t *pending)
{
	struct rro_batch b;
	uint32_t queued = 0;

	memset(&b, 0, sizeof(b));
	b.mode = ppe()[0];
	b.ppe_producer = ppe()[10];
	rro_ppe_hw_load(&b);
	for (uint32_t i = 0; i < frames; i++) {
		if (!native_ppe_queue(&b, i, 0x84e00000 + i * 2048, 100))
			break;
		queued++;
		++*pending;
		/* Never more queued than the ring holds minus one. */
		assert(*pending <= 63);
		assert(*pending == (b.ppe_producer - hw + 64) % 64);
	}
	native_ppe_flush(&b);
	rro_ppe_hw_save(&b);
	return queued;
}

int main(void)
{
	uint32_t pending = 0, reads, total = 0;

	(void)native_pn_service;
	(void)native_rx_security;
	(void)native_ppe_submit;
	map(AN7552_NPU_BASE, 4096);
	map(0x3e900000, 0x3000);
	map(0x442f0000, 0x10000);
	map(0x1fb50000, 4096);
	ppe()[0] = 2;
	for (uint32_t i = 0; i < 64; i++)
		slot(i)[1] = 0x80000000;
	RX_PPE_CACHE[1] = RX_PPE_STALE;

	/* Stale cache: the first batch reads the index once. */
	assert(batch(8, &pending) == 8 && hw_reads == 1);
	pending -= hw_consume(8);
	/* With a fast PPE the stale copy is refreshed only when the producer
	 * catches up with it: about once per lap of the 64-entry ring, not
	 * once per batch.
	 */
	reads = hw_reads;
	for (uint32_t i = 0; i < 1000; i++) {
		total += batch(4, &pending);
		pending -= hw_consume(4);
	}
	printf("fast PPE: %u frames, %u index reads\n", total, hw_reads - reads);
	assert(total == 4000 && hw_reads - reads <= total / 60);
	/* A slow PPE: the ring fills, the index is re-read, never overrun. */
	for (uint32_t i = 0; i < 2000; i++) {
		batch(16, &pending);
		pending -= hw_consume(i % 3 ? 3 : 0);
	}
	batch(16, &pending);
	assert(hw_reads > reads && pending == 63);
	while (pending)
		pending -= hw_consume(64);
	/* Someone else moved the producer (Linux reset, single submit). */
	ppe()[10] = 5;
	*(volatile uint32_t *)(uintptr_t)0x1fb50808 = 5;
	hw = 5;
	reads = hw_reads;
	assert(batch(1, &pending) == 1 && hw_reads == reads + 1);
	pending -= hw_consume(1);
	/* RRO disabled: the cache is dropped. */
	ppe()[0] = 2;
	{
		volatile uint32_t *q =
			(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);

		q[4] = 0;
		native_up_back();
		assert(RX_PPE_CACHE[1] == RX_PPE_STALE);
	}
	puts("PASS: PPE index cache: about one read per ring lap, re-read when full, never overruns");
	return 0;
}
