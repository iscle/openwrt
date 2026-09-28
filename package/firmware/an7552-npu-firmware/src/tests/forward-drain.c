/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../protocol.h"

static void barrier(void) {}
static uint32_t read32(uint32_t address)
{
	/* No radio register may be accessed while its queue is unavailable. */
	assert(address == AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	return *(volatile uint32_t *)(uintptr_t)address;
}

#include "../station.c.inc"
#include "../native-tx.c.inc"

static void map(uint32_t address, uint32_t size)
{
	assert(mmap((void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
	       (void *)(uintptr_t)address);
}

int main(void)
{
	volatile uint32_t *forward = (void *)(uintptr_t)
		(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *enable = (void *)(uintptr_t)
		(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);

	(void)native_transmit;
	map(AN7552_NPU_BASE, 4096);
	map(AN7552_FORWARD_WORK_NPU, AN7552_DIRECT_TOKENS * 16);
	*enable = 1;
	forward[0] = 1;
	forward[1] = 2;
	for (uint32_t band = 0; band < 2; band++) {
		volatile uint32_t *q = (void *)(uintptr_t)
			(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + band * 64);
		volatile uint32_t *work = (void *)(uintptr_t)
			(AN7552_FORWARD_WORK_NPU + band * AN7552_TDMA_COUNT * 16);
		uint32_t consumer = 0, active = 0;

		/* A flow can outlive the radio reset. Its packet has no radio owner. */
		work[0] = 1;
		native_forward(&consumer, &active, band);
		assert(work[0] == 3 && consumer == 1);
		assert(q[12] == 0 && forward[10] == 0);
		/* The same drain must work after queue setup reports an error. */
		q[5] = 1;
		q[7] = 1;
		work[4] = 1;
		native_forward(&consumer, &active, band);
		assert(work[4] == 3 && consumer == 2);
		/* A packet already handed to DMA must never be reclaimed here. */
		work[8] = 2;
		native_forward(&consumer, &active, band);
		assert(work[8] == 2 && consumer == 2);
	}
	assert(forward[12] == 4);
	puts("PASS: unavailable radio queues drain pending packets without MMIO or DMA reuse");
	return 0;
}
