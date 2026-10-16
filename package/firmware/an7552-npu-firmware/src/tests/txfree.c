/* SPDX-License-Identifier: MIT */
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
#include "../txfree.c.inc"
#define REGS 0x200d8000U

static void map_at(uint32_t address, size_t length)
{
	assert(mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)address);
}

/* native_txfree() on band 1: the event is read once into the stack, walked
 * there and copied to the Linux buffer word by word, including an odd tail.
 */
static void native_txfree_ring(uint32_t *work)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE +
						   AN7552_TXFREE_CONTROL + 64);
	volatile uint32_t *ring = (void *)(uintptr_t)0x442b2000U;
	volatile uint32_t *event = (void *)(uintptr_t)0x44d00000U;
	volatile uint32_t *host = (void *)(uintptr_t)0x44300000U;
	volatile uint8_t *copy = (void *)(uintptr_t)0x44310000U;
	volatile uint32_t *regs = (void *)(uintptr_t)REGS;

	map_at(AN7552_NPU_BASE, 4096);
	map_at(0x442b0000U, 0x4000);
	map_at(0x44d00000U, 0x1000);
	map_at(0x44300000U, 0x20000);
	map_at(0x1ec0d000U, 0x1000);
	map_at(REGS, 0x1000);
	for (uint32_t i = 0; i < AN7552_TXFREE_COUNT; i++) {
		ring[i * 4] = AN7552_TXFREE_BUFFER_HOST + 0x100000 + i * 2048;
		ring[i * 4 + 1] = 2048U << 16;
	}
	host[0] = 0x84310000U;
	host[1] = 2048U << 16;
	q[0] = 0x84300000U;
	q[1] = 16;
	q[2] = 0;
	q[3] = REGS;
	q[4] = 1;
	q[12] = 0;
	q[15] = 15;
	/* Nothing completed: the DMA index still equals ours. */
	native_txfree(1);
	assert(q[5] == 1 && !q[6] && !q[7]);
	/* Direct tokens 9 and 10 and host token 3000 complete in one event of
	 * 93 bytes, so the copy ends with an odd tail.
	 */
	for (uint32_t i = 0; i < 24; i++)
		event[i] = 0x01010101U * (i + 1);
	event[0] = (6U << 27) | (3U << 16);
	event[1] = 5U << 16;
	event[2] = 9 | (10U << 15);
	event[3] = 3000 | (32767U << 15);
	work[9 * 4] = 2;
	work[10 * 4] = 2;
	ring[1] = (1U << 31) | (1U << 30) | (93U << 16);
	regs[3] = 1;
	native_txfree(1);
	assert(q[7] == 1 && !q[6] && q[12] == 1 && q[2] == 1);
	assert(q[8] == 2 && q[9] == 1);
	assert(work[9 * 4] == 3 && work[10 * 4] == 3);
	assert(host[1] == ((1U << 31) | (1U << 30) | (93U << 16)));
	assert(!memcmp((void *)copy, (void *)event, 93));
	assert(!copy[93] && !copy[94] && !copy[95]);
	assert(ring[1] == 2048U << 16 && regs[2] == 0);
	assert(*(volatile uint32_t *)(uintptr_t)0x1ec0d198U == 1);
	/* The next slot is empty: nothing more is taken. */
	native_txfree(1);
	assert(q[7] == 1 && q[12] == 1);
}

int main(void)
{
	uint32_t q[16] = { 0 };
	uint32_t *work = mmap((void *)(uintptr_t)AN7552_FORWARD_WORK_NPU,
			      AN7552_DIRECT_TOKENS * 16, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			      -1, 0);
	uint32_t packet[8] = { (6U << 27) | (2U << 16), 5U << 16, 1U << 31,
			       1U << 30, 5 | (2048U << 15) };

	assert(work == (void *)(uintptr_t)AN7552_FORWARD_WORK_NPU);
	work[5 * 4] = 2;
	assert(!txfree_walk((void *)packet, 20, q, 0));
	assert(work[5 * 4] == 2);
	assert(!txfree_walk((void *)packet, 20, q, 1));
	assert(work[5 * 4] == 3 && q[8] == 1 && q[9] == 1);
	assert(txfree_walk((void *)packet, 20, q, 1) == 5);
	for (uint32_t length = 0; length < 20; length++)
		assert(txfree_walk((void *)packet, length, q, 0));
	packet[0] = (6U << 27) | (1U << 16);
	assert(txfree_walk((void *)packet, 20, q, 0) == 4);
	packet[4] = 5 | (32767U << 15);
	assert(!txfree_walk((void *)packet, 20, q, 0));
	packet[4] = 16384 | (32767U << 15);
	assert(txfree_walk((void *)packet, 20, q, 0) == 4);
	packet[4] = 5 | (32767U << 15);
	packet[1] = 4U << 16;
	assert(txfree_walk((void *)packet, 20, q, 0) == 2);
	packet[1] = 7U << 16;
	packet[3] = 1U << 31;
	assert(!txfree_walk((void *)packet, 20, q, 0));
	native_txfree_ring(work);
	assert(!munmap(work, AN7552_DIRECT_TOKENS * 16));
	puts("PASS: TXFREE bounds, counts, versions, host/direct tokens and double completion");
	puts("PASS: TXFREE ring: one uncached pass, word copy with an odd tail");
	return 0;
}
