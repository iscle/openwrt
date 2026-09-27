/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static void barrier(void)
{
}
static uint32_t read32(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

#include "../station.c.inc"
#include "../native-tx.c.inc"
#include "../txfree.c.inc"

static void map(uint32_t address, uint32_t size)
{
	assert(mmap((void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
		    0) == (void *)(uintptr_t)address);
}

static volatile uint32_t *queue(unsigned int band)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL +
				   band * 64);
}

static volatile uint32_t *station(unsigned int wcid)
{
	return (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
}

static volatile uint32_t *work(unsigned int token)
{
	return (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU + token * 16);
}

static volatile uint32_t *txwi(unsigned int token)
{
	return (void *)(uintptr_t)(AN7552_TXWI_NPU + token * 128);
}

static void setup(unsigned int token, unsigned int band, uint8_t tos)
{
	volatile uint8_t *packet =
		(void *)(uintptr_t)(AN7552_TDMA_BUFFER_NPU +
				    token * AN7552_TDMA_BUFFER_SIZE);
	volatile uint32_t *entry = work(token);

	memset((void *)packet, 0, 64);
	packet[0] = 2;
	packet[12] = 8;
	packet[14] = 0x45;
	packet[15] = tos;
	entry[1] = 34;
	entry[2] = ((10 + band) << 14) | (band << 25);
	entry[3] = 2 + band;
	entry[0] = 1;
}

int main(void)
{
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	uint32_t consumer = 0, active = 0, complete[3], stats[16] = { 0 };

	(void)native_transmit;
	(void)native_txfree;
	map(AN7552_NPU_BASE, 4096);
	map(AN7552_FORWARD_WORK_NPU, 0x10000);
	map(AN7552_STATION_NPU, 0x10000);
	map(AN7552_QOS_NPU, 0x8000);
	map(AN7552_TDMA_BUFFER_NPU, 0x400000);
	map(AN7552_TXWI_NPU, 0x40000);
	map(0x44240000, 0x10000);
	map(0x200d4000, 0x5000);
	*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
					  AN7552_TDMA_CONTROL) = 1;
	forward[0] = 1;
	forward[1] = 2;
	for (unsigned int band = 0; band < 2; band++) {
		volatile uint32_t *s = station(10 + band);
		volatile uint8_t *qos =
			(void *)(uintptr_t)(AN7552_QOS_NPU +
					    (2 + band) * AN7552_QOS_STRIDE);

		queue(band)[5] = 1;
		s[0] = AN7552_STATION_AUTHORIZED | AN7552_STATION_MLO;
		s[1] = band | ((2 + band) << 8);
		s[2] = 2;
		s[4] = 0x80;
		s[5] = 11 | (2U << 24);
		s[6] = 12 | (1U << 16) | (3U << 24);
		s[7] = 1;
		qos[40] = 5;
		((volatile uint32_t *)qos)[16] = 1;
	}
	/* Same flow, changed DSCP: both queues must receive their own packet. */
	setup(0, 0, 0);
	setup(1, 0, 160);
	native_forward(&consumer, &active, 0);
	assert(consumer == 2 && work(0)[0] == 2 && work(1)[0] == 2);
	assert(queue(0)[4] == 1 && queue(1)[4] == 1);
	assert(txwi(0)[9] == (1U << 24 | 10U << 8 | 2));
	assert(txwi(1)[9] == (1U << 24 | 11U << 8 | 3));
	assert(txwi(1)[8] >> 16 == 1);
	assert(txwi(1)[10] ==
	       AN7552_TDMA_BUFFER_HOST + AN7552_TDMA_BUFFER_SIZE);
	/* A completion on the other radio still releases the source pool token. */
	complete[0] = 6U << 27 | 1U << 16;
	complete[1] = 5U << 16;
	complete[2] = 32767U << 15 | 1;
	assert(!txfree_walk((void *)complete, sizeof(complete), stats, 1));
	assert(work(1)[0] == 3 && work(0)[0] == 2);
	/* Full destination queue must retain work and its token for retry. */
	setup(2, 0, 160);
	*(volatile uint32_t *)(uintptr_t)0x200d845c = 2;
	native_forward(&consumer, &active, 0);
	assert(consumer == 2 && work(2)[0] == 1 && queue(1)[4] == 1);
	*(volatile uint32_t *)(uintptr_t)0x200d845c = 0;
	native_forward(&consumer, &active, 0);
	assert(consumer == 3 && work(2)[0] == 2 && queue(1)[4] == 2);
	/* Revocation and WCID reuse cannot authorize the selected peer. */
	station(11)[0] = 0;
	setup(3, 0, 160);
	native_forward(&consumer, &active, 0);
	assert(work(3)[0] == 3 && queue(1)[4] == 2);
	station(11)[0] = 3;
	station(11)[2] = 4;
	setup(4, 0, 160);
	native_forward(&consumer, &active, 0);
	assert(work(4)[0] == 3 && queue(1)[4] == 2);
	station(11)[2] = 2;
	station(11)[1] = 1 | (4U << 8);
	setup(5, 0, 160);
	native_forward(&consumer, &active, 0);
	assert(work(5)[0] == 3 && queue(1)[4] == 2);
	station(11)[1] = 1 | (3U << 8);
	/* Reverse routing keeps a band-1 token, even on the band-0 DMA ring. */
	setup(AN7552_TDMA_COUNT, 1, 0);
	consumer = active = 0;
	native_forward(&consumer, &active, 1);
	assert(work(AN7552_TDMA_COUNT)[0] == 2);
	assert(txwi(AN7552_TDMA_COUNT)[8] >> 16 == AN7552_TDMA_COUNT);
	assert(txwi(AN7552_TDMA_COUNT)[9] == (1U << 24 | 10U << 8 | 2));
	puts("PASS: per-packet MLO routing, queue pressure, cross-radio completion and peer revocation");
	return 0;
}
