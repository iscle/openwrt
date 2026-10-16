/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static int revoke_during_identity_check;
static int ppe_ready;

static void barrier(void)
{
	__sync_synchronize();
}

static uint32_t read32(uint32_t address)
{
	/* A single token-0 PPE completion, optionally delayed by the test. */
	if (address == 0x1fb50fe4) {
		volatile uint32_t *ppe =
			(void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);

		return ppe_ready && *(volatile uint32_t *)0x1fb50808 == 1 &&
		       !ppe[2];
	}
	if (address == 0x1fb50fe0)
		return 0x80000000;

	return *(volatile uint32_t *)(uintptr_t)address;
}

__attribute__((unused)) static uint32_t packet_word(volatile uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"

__attribute__((unused)) static uint32_t packet_mac_low(volatile uint8_t *p)
{
	return packet_word(p);
}

__attribute__((unused)) static uint32_t packet_mac_high(volatile uint8_t *p)
{
	/* Model Linux revoking authorization after station[0] was read. */
	if (revoke_during_identity_check)
		*(volatile uint32_t *)(uintptr_t)(AN7552_STATION_NPU + 64) = 0;
	return p[4] | (uint32_t)p[5] << 8;
}

/* Model Linux revoking authorization after the identity check passed. */
#define RX_TEST_AFTER_IDENTITY_CHECK()                                        \
	do {                                                                  \
		if (revoke_during_identity_check)                             \
			*(volatile uint32_t *)(uintptr_t)(AN7552_STATION_NPU + \
							  64) = 0;            \
	} while (0)
/* Host tests only map the uncached alias. */
#define RX_HEADER_ALIAS(host) ((volatile uint8_t *)rx_words(host))
#include "../rro.c.inc"
#include "../ppe.c.inc"

static const struct {
	uint32_t address;
	size_t length;
} regions[] = {
	{ AN7552_NPU_BASE, 4096 },
	{ 0x44110000, 0xa0000 },
	{ 0x441b0000, 0x30000 },
	{ 0x442a0000, 0x10000 },
	{ 0x442c0000, 0x40000 },
	{ 0x44e00000, 4096 },
	{ 0x49000000, 0x30000 },
	{ 0x49200000, 4096 },
	{ 0x49300000, 4096 },
	{ 0x200d4000, 0x5000 },
	{ 0x1ec0d000, 4096 },
	{ 0x1fb50000, 4096 },
};

static volatile uint32_t *control(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
}

static volatile uint32_t *ppe_control(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
}

static volatile uint32_t *data_control(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL);
}

static void reset(void)
{
	volatile uint32_t *q = control();

	for (unsigned int i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
		memset((void *)(uintptr_t)regions[i].address, 0, regions[i].length);
	revoke_during_identity_check = 0;
	ppe_ready = 0;
	q[0] = 0x89010000;
	q[1] = AN7552_RX_RING_COUNT;
	q[4] = 1;
	q[18] = 0x89300000;
	*rx_words(q[18]) = AN7552_RX_RING_COUNT - 2;
	for (uint32_t band = 0; band < 2; band++) {
		volatile uint32_t *data = data_control() + band * 16;

		data[0] = 0x89000000 + band * 0x8000;
		data[1] = AN7552_RX_RING_COUNT;
		data[4] = 0x200d4500 + band * 0x4000;
		data[5] = AN7552_RX_RING_COUNT - 2;
		data[6] = AN7552_RX_RING_COUNT - 1;
		data[11] = 1800;
	}
	for (uint32_t i = 0; i < AN7552_RX_RING_COUNT; i++)
		rx_words(AN7552_RX_COMPLETE_HOST)[i * 4 + 3] = 15U << 28;
	for (uint32_t i = 0; i < 2; i++) {
		volatile uint32_t *host = rx_words(0x89000000) + i * 4;

		host[0] = 0x89200000 + i * 256;
		host[1] = 1800U << 16;
		host[2] = (1234 + i) << 16;
	}
	ppe_control()[0] = 2;
	rx_words(0x842f0000)[1] = 0x80000000;
	pn_entry(2)[0] = 1;
	pn_entry(2)[1] = 9;
	*rx_words(AN7552_STATION_HOST + 64) = 1;
}

static void packet(uint32_t token, int fragment)
{
	volatile uint32_t *p = rx_words(AN7552_RX_BUFFER_HOST + token * 2048);
	volatile uint8_t *bytes = (void *)p;
	volatile uint32_t *ring = rx_words(AN7552_RX_COMPLETE_HOST) + token * 4;

	/* Authorized CCMP TCP data, station 2, TID 5, ascending sequence/PN. */
	p[0] = 0x10000000 | 160;
	p[1] = 0x800f0002;
	p[2] = 0x00040080;
	p[3] = 1U << 16;
	p[8] = 0x4188;
	p[10] = (5U << 16) | ((0x123 + token) << 4);
	p[12] = token + 1;
	bytes[96] = 2;
	bytes[108] = 8;
	bytes[110] = 0x45;
	bytes[113] = 50;
	bytes[119] = 6;
	bytes[142] = 0x50;
	if (fragment)
		bytes[116] = 0x20;
	ring[0] = AN7552_RX_BUFFER_HOST + token * 2048;
	ring[1] = (160U << 16) | (1U << 30);
	ring[2] = (AN7552_RX_TOKEN_BASE + token) << 16;
	ring[3] = 0;
	rx_words(AN7552_RX_STATE_HOST)[token] = 1;
}

static void test_revocation(void)
{
	reset();
	packet(0, 0);
	revoke_during_identity_check = 1;
	native_rro_receive();
	assert(!*rx_words(AN7552_STATION_HOST + 64));
	assert(!ppe_control()[1] && !ppe_control()[12]);
	assert(control()[2] == 1 && control()[7] == 1);
	assert(!control()[6] && !rx_words(AN7552_RX_STATE_HOST)[0]);
}

static void assert_ordered_delivery(void)
{
	assert(control()[2] == 2 && control()[7] == 2);
	assert(ppe_control()[1] == 1 && ppe_control()[2] == 1);
	assert(!ppe_control()[12] && !ppe_control()[3] && !control()[6]);
	assert(rx_words(0x89200000)[12] == 1);
	assert(rx_words(0x89200100)[12] == 2);
	assert(!rx_words(AN7552_RX_STATE_HOST)[0]);
	assert(!rx_words(AN7552_RX_STATE_HOST)[1]);
	/* Retries must reuse saved security checks, not reject the same PN. */
	assert((rx_words(0x842f9000)[0] & 15) == 1);
	assert((rx_words(0x842f9010)[0] & 15) == 1);
}

static void test_ordering(int delayed, int host_full)
{
	reset();
	packet(0, 0);
	packet(1, 1);
	ppe_ready = !delayed;
	if (host_full)
		data_control()[5] = 0;
	native_rro_receive();
	if (delayed || host_full) {
		assert(!control()[2] && control()[16] == 1);
		assert(ppe_control()[12] == 1);
		assert((rx_words(AN7552_RX_STATE_HOST)[0] & 255) == 2);
		assert((rx_words(AN7552_RX_STATE_HOST)[1] & 255) == 1);
		/* No progress must remain bounded and leave both buffers owned. */
		native_rro_receive();
		assert(!control()[2] && ppe_control()[12] == 1);
		ppe_ready = 1;
		data_control()[5] = AN7552_RX_RING_COUNT - 2;
		native_rro_receive();
	}
	/* A main-loop service also exposes overtaking in the unfixed code. */
	native_ppe_service();
	assert_ordered_delivery();
}

static void test_security_accounting(int ppe_full)
{
	reset();
	packet(0, !ppe_full);
	if (ppe_full)
		rx_words(0x842f0000)[1] = 0;
	else
		data_control()[5] = 0;
	native_rro_receive();
	assert(!control()[7] && !control()[2]);
	assert(control()[23] == 1);
	native_rro_receive();
	assert(!control()[7] && control()[23] == 1);
	/* Complete the cached packet without losing or doubling its count. */
	rx_words(0x842f0000)[1] = 0x80000000;
	data_control()[5] = AN7552_RX_RING_COUNT - 2;
	native_rro_receive();
	ppe_ready = 1;
	native_ppe_service();
	assert(control()[7] == 1 && control()[2] == 1);
	assert(control()[23] == 1 && !control()[24] && !control()[6]);
	assert((rx_words(0x842f9000)[0] & 15) == 1);
	native_rro_receive();
	assert(control()[23] == 1);
}

/* A buffer following a non-final one carries payload, not a descriptor:
 * even if its bytes look like a valid secure frame, it must not be parsed,
 * must not advance replay counters and must reach Linux in order.
 */
static void test_continuation(void)
{
	volatile uint32_t *ring = rx_words(AN7552_RX_COMPLETE_HOST);
	uint32_t low, high;

	reset();
	ppe_ready = 1;
	packet(0, 0);
	packet(1, 0);
	ring[1] = 160U << 16;			/* first buffer, more follows */
	rx_words(AN7552_RX_BUFFER_HOST + 2048)[12] = 0x7fffffff;	/* bogus PN */
	low = pn_entry(2)[4 + 5 * 2];
	high = pn_entry(2)[5 + 5 * 2];
	native_rro_receive();
	native_ppe_service();
	assert(control()[7] == 2 && !control()[6]);
	assert(!ppe_control()[1]);		/* nothing went to the PPE */
	assert(control()[2] == 2);		/* both delivered to Linux */
	/* Only the first buffer's real PN (1) may be recorded. */
	assert(pn_entry(2)[4 + 5 * 2] == 1 && pn_entry(2)[5 + 5 * 2] == 0);
	(void)low;
	(void)high;
	assert(!control()[30]);			/* chain closed by the last buffer */
}

int main(void)
{
	for (unsigned int i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
		void *address = (void *)(uintptr_t)regions[i].address;

		assert(mmap(address, regions[i].length, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			    -1, 0) == address);
	}
	(void)native_pn_service;
	test_revocation();
	test_ordering(0, 0);
	test_ordering(1, 0);
	test_ordering(0, 1);
	test_security_accounting(0);
	test_security_accounting(1);
	test_continuation();
	puts("PASS: RX batch revocation, ordering, backpressure, security accounting and continuation buffers");
	return 0;
}
