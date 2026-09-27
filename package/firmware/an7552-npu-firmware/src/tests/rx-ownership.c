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
	if (address == 0x1fb50fe4 &&
	    *(volatile uint32_t *)(uintptr_t)0x1fb50fe0 == 0x80000000)
		return 0;
	return *(volatile uint32_t *)(uintptr_t)address;
}
static uint32_t packet_word(volatile uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"
#include "../station.c.inc"
#include "../rro.c.inc"
#include "../ppe.c.inc"
static void map(uint32_t address, size_t length)
{
	assert(mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)address);
}
int main(void)
{
	volatile uint32_t *q, *data, *ring, *host, *free_ids, *states;
	volatile uint8_t *packet;
	map(AN7552_NPU_BASE, 4096);
	map(0x44010000, 0xf0000);
	map(0x44110000, 0xa0000);
	map(0x44300000, 0x100000);
	map(0x441b0000, 0x60000);
	map(0x442c0000, 0x40000);
	map(0x44e00000, 0xc00000);
	map(0x49000000, 0x30000);
	map(0x49200000, 4096);
	map(0x49300000, 4096);
	map(0x200d4000, 0x5000);
	map(0x1ec0d000, 4096);
	map(0x1fb50000, 4096);
	(void)native_pn_service;
	(void)station_learn;
	q = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	q[0] = 0x89010000;
	q[1] = AN7552_RX_RING_COUNT;
	q[4] = 1;
	q[18] = 0x89300000;
	q[20] = 1;
	q[21] = 0;
	q[22] = AN7552_RX_BUFFER_COUNT - 1;
	*rx_words(q[18]) = AN7552_RX_RING_COUNT - 2;
	free_ids = rx_words(AN7552_RX_FREE_HOST);
	states = rx_words(AN7552_RX_STATE_HOST);
	for (uint32_t i = 0; i < AN7552_RX_BUFFER_COUNT; i++)
		free_ids[i] = i;
	states[0] = 1;
	for (uint32_t band = 0; band < 2; band++) {
		data = (void *)(uintptr_t)(AN7552_NPU_BASE +
					   AN7552_RX_DATA_CONTROL + band * 64);
		data[0] = 0x89000000 + band * 0x8000;
		data[1] = AN7552_RX_RING_COUNT;
		data[4] = 0x200d4500 + band * 0x4000;
		data[5] = AN7552_RX_RING_COUNT - 2;
		data[6] = AN7552_RX_RING_COUNT - 1;
		data[11] = 1800;
	}
	ring = rx_words(AN7552_RX_COMPLETE_HOST);
	for (uint32_t i = 0; i < AN7552_RX_RING_COUNT; i++)
		ring[i * 4 + 3] = 15U << 28;
	ring[0] = AN7552_RX_BUFFER_HOST;
	ring[1] = (64U << 16) | (1U << 30);
	ring[2] = AN7552_RX_TOKEN_BASE << 16;
	ring[3] = 0;
	host = rx_words(0x89000000);
	host[0] = 0x89200000;
	host[1] = 1800U << 16;
	host[2] = 1234U << 16;
	packet = (void *)rx_words(AN7552_RX_BUFFER_HOST);
	for (uint32_t i = 0; i < 64; i++)
		packet[i] = i;
	native_rro_receive();
	assert(!q[6] && q[7] == 1 && q[2] == 1 && q[16] == 1);
	assert(!memcmp((void *)packet, (void *)rx_words(0x89200000), 64));
	assert((rx_words(q[0])[2] >> 16) == 1234);
	assert(states[0] == 0 && free_ids[0] == 0 && q[21] == 1 &&
	       q[22] == AN7552_RX_BUFFER_COUNT);
	assert(read32(0x1ec0d1a8) == 1);
	native_rro_receive();
	assert(q[7] == 1);
	*(volatile uint32_t *)(uintptr_t)0x200d450c = 1;
	native_rro_receive();
	assert(states[1] == 1 && q[20] == 2);
	assert(rx_words(AN7552_RX_DATA_RING_HOST +
			(AN7552_RX_RING_COUNT - 1) * 16)[0] ==
	       AN7552_RX_BUFFER_HOST + 2048);
	data = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL);
	assert(data[9] == 1);
	*(volatile uint32_t *)(uintptr_t)0x200d450c = 2;
	native_rx_refill(0);
	assert(rx_words(AN7552_RX_DATA_RING_HOST)[3] == (1U << 28));
	volatile uint32_t *ppe = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	ppe[0] = 2;
	rx_words(0x842f0000)[1] = 0x80000000;
	states[42] = 2;
	/* The legacy untagged mode must not submit a packet. */
	assert(!native_ppe_submit(42, AN7552_RX_BUFFER_HOST + 42 * 2048, 64));
	assert(!ppe[12] && !ppe[1]);
	native_ppe_service();
	assert(ppe[3] == 1);
	/* Seed an in-flight completion to test ownership independently. */
	ppe[0] = AN7552_PPE_MODE_BRIDGE;
	ppe[3] = 0;
	ppe[12] = 1;
	volatile uint32_t *saved = rx_metadata(42);
	saved[0] = AN7552_RX_BUFFER_HOST + 42 * 2048;
	saved[1] = (64U << 16) | (1U << 30);
	saved[2] = (AN7552_RX_TOKEN_BASE + 42) << 16;
	saved[7] = 64;
	memset((void *)rx_words(0x84010000 + 42 * 160), 7, 64);
	memset((void *)rx_words(saved[0]), 7, 64);
	q[2] = 0;
	data[2] = 0;
	host[1] = 1800U << 16;
	*(volatile uint32_t *)(uintptr_t)0x1fb50fe4 = 1;
	*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0xbf00002a;
	*(volatile uint32_t *)(uintptr_t)0x1fb50fdc = 0x000f0005;
	native_ppe_service();
	assert(!ppe[3] && !ppe[12] && ppe[15] == 1 && !states[42]);
	assert(rx_words(0x842f9000)[2] == 0x200f0005);
	assert(*(volatile uint8_t *)rx_words(0x89200000) == 7);
	states[43] = 2;
	ppe[12] = 1;
	*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0xc000002b;
	native_ppe_service();
	assert(!ppe[3] && !ppe[12] && ppe[14] == 1 && !states[43]);
	*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0xc000002b;
	native_ppe_service();
	assert(ppe[3] == 4);
	/* Actual mode-4 submission/miss, including a full CPU completion ring.
	 * The internal header stays DMA-owned until the result can be copied.
	 */
	uint8_t original[128];
	ppe[0] = AN7552_PPE_MODE_BRIDGE;
	ppe[3] = 0;
	ppe[10] = 0;
	ppe[12] = 0;
	rx_words(0x842f0000)[1] = 0x80000000;
	states[42] = 2;
	q[22] = AN7552_RX_BUFFER_COUNT - 1;
	q[2] = 0;
	data[2] = 0;
	host[1] = 1800U << 16;
	saved[0] = AN7552_RX_BUFFER_HOST + 42 * 2048;
	saved[1] = (128U << 16) | (1U << 30);
	saved[4] = (2047U << 4) | 1;
	saved[6] = 64;
	saved[7] = 64;
	packet = (void *)rx_words(saved[0]);
	for (unsigned int i = 0; i < sizeof(original); i++)
		packet[i] = i;
	packet[64] = 2;
	packet[76] = 8;
	packet[77] = 0;
	memcpy(original, (void *)packet, sizeof(original));
	/* Invalid producer/token values cannot reach descriptor memory. */
	ppe[10] = 64;
	assert(!native_ppe_submit(42, saved[0] + 64, 64));
	assert(ppe[3] == 2 && !ppe[12]);
	ppe[10] = 0;
	ppe[3] = 0;
	assert(native_ppe_submit(AN7552_RX_BUFFER_COUNT, saved[0] + 64, 64) ==
	       -1);
	assert(!ppe[12]);
	assert(native_ppe_submit(42, saved[0] + 64, 64) == 1);
	assert(rx_words(0x842f0000)[2] == saved[0] + 60);
	assert(rx_words(0x842f0000)[1] == 68);
	assert(packet[72] == 0x81 && packet[74] == 8 && packet[75] == 0);
	assert(rx_domain(42)->active);
	*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0xbf00002a;
	*rx_words(q[18]) = 0;
	native_ppe_service();
	assert(ppe[12] == 1 && states[42] == 2 && rx_domain(42)->active);
	*rx_words(q[18]) = AN7552_RX_RING_COUNT - 2;
	native_ppe_service();
	assert(!ppe[12] && !states[42] && !rx_domain(42)->active);
	assert(!memcmp((void *)packet, original, sizeof(original)));
	assert(!memcmp((void *)rx_words(0x89200000), original,
		       sizeof(original)));
	assert(rx_words(0x842f9000)[2] == 0x200f0005);

	/* Continuation data can mimic a valid RXD, including an active WCID.
	 * Neither part of a split frame may independently consume a PN.
	 */
	volatile uint32_t *entry = pn_entry(2);
	memset((void *)entry, 0, 256);
	const uint32_t initial[34] = { 5 };
	assert(pn_install(2, 1, 0, initial));
	entry[1] = 7;
	q[2] = q[3] = q[6] = q[16] = q[17] = q[30] = 0;
	q[22] = AN7552_RX_BUFFER_COUNT - 3;
	ppe[0] = 0;
	data[2] = 0;
	data[6] = AN7552_RX_RING_COUNT - 1;
	*(volatile uint32_t *)(uintptr_t)0x200d450c = 0;
	for (uint32_t i = 0; i < 3; i++) {
		volatile uint32_t *words;
		uint32_t address = AN7552_RX_BUFFER_HOST + i * 2048;

		states[i] = 1;
		ring[i * 4] = address;
		ring[i * 4 + 1] = (96U << 16) | (i ? 1U << 30 : 0);
		ring[i * 4 + 2] = (AN7552_RX_TOKEN_BASE + i) << 16;
		ring[i * 4 + 3] = 0;
		host[i * 4] = 0x89200000;
		host[i * 4 + 1] = 1800U << 16;
		host[i * 4 + 2] = (1234 + i) << 16;
		words = rx_words(address);
		memset((void *)words, 0, 96);
		words[0] = (2U << 27) | 96;
		words[1] = 2 | (1U << 16) | (1U << 19);
		words[2] = (4U << 16) | (1U << 7);
		words[8] = 0x0088;
		words[12] = 100 + i;
	}
	native_rro_receive();
	assert(!q[6] && q[16] == 1);
	assert(pn_domain_lookup(entry[2], entry[3])[4] == 5);
	assert(rx_words(0x842f9000)[0] == 0);
	/* Backpressure must retain continuation state until publication. */
	host[5] |= 1U << 31;
	native_rro_receive();
	assert(q[6] == 4 && q[16] == 1 &&
	       pn_domain_lookup(entry[2], entry[3])[4] == 5);
	host[5] &= ~(1U << 31);
	q[6] = 0;
	native_rro_receive();
	assert(q[16] == 2 && pn_domain_lookup(entry[2], entry[3])[4] == 5);
	assert(rx_words(0x842f9000 + 16)[0] == 0);
	/* The host validates the assembled frame exactly once. */
	assert(pn_compare(2, 0, 100, 0) == 1);
	native_rro_receive();
	assert(!q[6] && q[16] == 3 &&
	       pn_domain_lookup(entry[2], entry[3])[4] == 102);
	assert((rx_words(0x842f9000 + 32)[0] & 15) == 1);
	puts("PASS: RX ownership, refill, split-frame replay and backpressure");
	return 0;
}
