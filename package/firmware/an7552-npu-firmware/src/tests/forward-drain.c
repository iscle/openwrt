/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../protocol.h"

static void barrier(void)
{
}
static uint32_t read32(uint32_t address)
{
	/* No radio register may be accessed while its queue is unavailable. */
	assert(address == AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	return *(volatile uint32_t *)(uintptr_t)address;
}

#include "../tdma.c.inc"
#include "../tx-pool.c.inc"
#include "../station.c.inc"
#include "../native-tx.c.inc"

static void map(uint32_t address, uint32_t size)
{
	assert(mmap((void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
		    0) == (void *)(uintptr_t)address);
}

int main(void)
{
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *enable =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);

	(void)native_transmit;
	(void)tx_pool_complete;
	map(AN7552_NPU_BASE, 4096);
	map(TX_POOL_ADDR, 0xc000);
	map(AN7552_TDMA_RING_NPU, 0x10000);
	map(0x1fb50000, 4096);
	tx_pool_boot();
	map(AN7552_FORWARD_WORK_NPU, AN7552_DIRECT_TOKENS * 16);
	*enable = 1;
	forward[0] = 1;
	forward[1] = 2;
	for (uint32_t band = 0; band < 2; band++) {
		volatile uint32_t *q =
			(void *)(uintptr_t)(AN7552_NPU_BASE +
					    AN7552_NATIVE_CONTROL + band * 64);
		volatile uint32_t *work =
			(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
					    band * AN7552_TDMA_COUNT * 32);
		struct tx_band b = { 0 };
		volatile uint32_t *tdma = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_STRIDE);

		/* A flow can outlive the radio reset. Its packet has no radio owner. */
		work[1] = 0x80000000 | 64;
		work[2] = TX_BUFFER_HOST(band * AN7552_TDMA_COUNT);
		work[4] = band << 25;
		native_download(tdma, &b, band);
		assert(b.consumer == 1 && tx_pool()->completed == band * 2 + 1);
		assert(q[12] == 0 && forward[10] == 0);
		/* The same drain must work after queue setup reports an error. */
		q[5] = 1;
		q[7] = 1;
		work[9] = 0x80000000 | 64;
		work[10] = TX_BUFFER_HOST(band * AN7552_TDMA_COUNT + 1);
		work[12] = band << 25;
		native_download(tdma, &b, band);
		assert(b.consumer == 2);
		/* A packet already handed to DMA must never be reclaimed here. */
		work[17] = 64;
		native_download(tdma, &b, band);
		assert(work[17] == 64 && b.consumer == 2);
	}
	assert(forward[12] == 4);
	puts("PASS: unavailable radio queues drain pending packets without MMIO or DMA reuse");
	return 0;
}
