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
int main(void)
{
	(void)native_txfree;
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
	assert(!munmap(work, AN7552_DIRECT_TOKENS * 16));
	puts("PASS: TXFREE bounds, counts, versions, host/direct tokens and double completion");
	return 0;
}
