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
#include "../tdma.c.inc"
#include "../tx-pool.c.inc"
#include "../txfree.c.inc"
int main(void)
{
	(void)native_txfree;
	(void)tdma_receive;
	(void)tx_pool_boot;
	(void)tx_pool_start;
	(void)tx_pool_stop;
	(void)tx_pool_arm;
	uint32_t q[16] = { 0 };
	uint32_t *work = mmap((void *)(uintptr_t)TX_POOL_ADDR,
			      0xc000, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			      -1, 0);
	uint32_t packet[8] = { (6U << 27) | (2U << 16), 5U << 16, 1U << 31,
			       1U << 30, 5 | (2048U << 15) };

	assert(work == (void *)(uintptr_t)TX_POOL_ADDR);
	tx_pool()->token_buffer[5] = TX_TOKEN_VALID | 5;
	tx_pool()->inflight[0] = 1;
	assert(!txfree_walk((void *)packet, 20, q, 0));
	assert(tx_pool_token_busy(5));
	assert(!txfree_walk((void *)packet, 20, q, 1));
	assert(!tx_pool_token_busy(5) && q[8] == 1 && q[9] == 1);
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
	assert(!munmap(work, 0xc000));
	puts("PASS: TXFREE bounds, counts, versions, host/direct tokens and double completion");
	return 0;
}
