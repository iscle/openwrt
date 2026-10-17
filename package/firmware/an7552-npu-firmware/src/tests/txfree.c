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
#define REGS 0x200d8000U

static void *map_at(uint32_t address, size_t length, int prot)
{
	void *p = mmap((void *)(uintptr_t)address, length, prot,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	assert(p == (void *)(uintptr_t)address);
	return p;
}

/* native_txfree() on band 1: an empty slot must be detected from its done
 * bit without touching the radio's DMA index (PCIe); an event is taken only
 * when the done bit and the DMA index agree; the event is read once into the
 * stack and copied to the Linux buffer word by word, including an odd tail.
 */
static void native_txfree_ring(void)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE +
						   AN7552_TXFREE_CONTROL + 64);
	volatile uint32_t *ring = (void *)(uintptr_t)0x442b2000U;
	volatile uint32_t *event = (void *)(uintptr_t)0x44d00000U;
	volatile uint32_t *host = (void *)(uintptr_t)0x44300000U;
	volatile uint8_t *copy = (void *)(uintptr_t)0x44310000U;
	volatile uint32_t *regs = (void *)(uintptr_t)REGS;
	struct tx_pool *p = tx_pool();
	uint32_t completed = p->completed;

	map_at(0x442b0000U, 0x4000, PROT_READ | PROT_WRITE);
	map_at(0x44d00000U, 0x1000, PROT_READ | PROT_WRITE);
	map_at(0x44300000U, 0x20000, PROT_READ | PROT_WRITE);
	map_at(0x1ec0d000U, 0x1000, PROT_READ | PROT_WRITE);
	map_at(REGS, 0x1000, PROT_NONE);
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
	/* Empty slot: a DMA index read would fault on the PROT_NONE page. */
	for (uint32_t i = 0; i < 100; i++)
		native_txfree(1);
	assert(q[5] == 1 && !q[6] && !q[7]);
	assert(!mprotect((void *)(uintptr_t)REGS, 0x1000,
			 PROT_READ | PROT_WRITE));
	/* Direct tokens 9 and 10 (buffers 2200 and 2201, band 1) and host
	 * token 3000 complete in one event of 93 bytes.
	 */
	p->token_buffer[9] = TX_TOKEN_VALID | TX_TOKEN_BAND | 2200;
	p->token_buffer[10] = TX_TOKEN_VALID | TX_TOKEN_BAND | 2201;
	p->inflight[1] = 2;
	p->free_tokens -= 2;
	p->free_buffers -= 2;
	for (uint32_t i = 0; i < 24; i++)
		event[i] = 0x01010101U * (i + 1);
	event[0] = (6U << 27) | (3U << 16);
	event[1] = 5U << 16;
	event[2] = 9 | (10U << 15);
	event[3] = 3000 | (32767U << 15);
	/* Done bit set but the DMA index has not moved: not taken yet. */
	ring[1] = (1U << 31) | (1U << 30) | (93U << 16);
	regs[3] = 0;
	native_txfree(1);
	assert(!q[7] && !q[6] && tx_pool_token_busy(9));
	regs[3] = 1;
	native_txfree(1);
	assert(q[7] == 1 && !q[6] && q[12] == 1 && q[2] == 1);
	assert(q[8] == 2 && q[9] == 1);
	assert(!tx_pool_token_busy(9) && !tx_pool_token_busy(10));
	assert(p->completed == completed + 2 && p->inflight[1] == 0);
	assert(host[1] == ((1U << 31) | (1U << 30) | (93U << 16)));
	assert(!memcmp((void *)copy, (void *)event, 93));
	assert(!copy[93] && !copy[94] && !copy[95]);
	assert(ring[1] == 2048U << 16 && regs[2] == 0);
	assert(*(volatile uint32_t *)(uintptr_t)0x1ec0d198U == 1);
	/* The next slot is empty: nothing more is taken, no index read. */
	assert(!mprotect((void *)(uintptr_t)REGS, 0x1000, PROT_NONE));
	native_txfree(1);
	assert(q[7] == 1 && q[12] == 1);
}

int main(void)
{
	(void)tdma_receive;
	(void)tx_pool_arm;
	(void)tx_pool_stop;
	uint32_t q[16] = { 0 };
	void *npu = mmap((void *)(uintptr_t)AN7552_NPU_BASE, 4096,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	void *pool = mmap((void *)(uintptr_t)TX_POOL_ADDR, 0xc000,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	void *txwi = mmap((void *)(uintptr_t)AN7552_TXWI_NPU, 0x40000,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	struct tx_pool *p;
	uint32_t packet[8] = { (6U << 27) | (2U << 16), 5U << 16, 1U << 31,
			       1U << 30, 5 | (2048U << 15) };

	assert(npu == (void *)(uintptr_t)AN7552_NPU_BASE);
	assert(pool == (void *)(uintptr_t)TX_POOL_ADDR);
	assert(txwi == (void *)(uintptr_t)AN7552_TXWI_NPU);
	tx_pool_boot();
	tx_pool_start(1, 0);
	p = tx_pool();
	assert(p->free_tokens == TX_POOL_TOKENS && p->free_buffers == 992);
	/* Token 5 is queued to the radio with buffer 2100 on band 1. */
	p->token_buffer[5] = TX_TOKEN_VALID | TX_TOKEN_BAND | 2100;
	p->inflight[1] = 1;
	p->free_tokens--;
	p->free_buffers--;
	assert(txfree_walk((void *)packet, 20, q, 0) == 0);
	packet[4] = 6 | (2048U << 15);
	assert(txfree_walk((void *)packet, 20, q, 0) == 5);
	packet[4] = 5 | (2048U << 15);
	assert(!txfree_walk((void *)packet, 20, q, 1));
	tx_pool_publish(p);
	assert(q[8] == 1 && q[9] == 1 && forward[9] == 1);
	assert(!tx_pool_token_busy(5) && p->inflight[1] == 0);
	assert(p->free_tokens == TX_POOL_TOKENS && p->free_buffers == 992);
	assert(p->buffer[991] == 2100 && p->token[2047] == 5);
	assert(txfree_walk((void *)packet, 20, q, 1) == 5);
	assert(txfree_walk((void *)packet, 20, q, 0) == 5);
	p->token_buffer[5] = TX_TOKEN_VALID | 7;
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
	native_txfree_ring();
	puts("PASS: TXFREE bounds, counts, versions, host/direct tokens and double completion");
	puts("PASS: TXFREE ring: empty slot needs no DMA index read, done bit and index must agree, one-pass word copy with an odd tail");
	return 0;
}
