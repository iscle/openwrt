/* SPDX-License-Identifier: MIT */
/* Upload pipeline, both stages on one thread: the rx-batch scenarios
 * (revocation, ordering behind the PPE, host ring back-pressure, security
 * accounting, continuation buffers, per-station identity) give the same
 * results through core 0's front half and core 1's back half. Plus the cases
 * the cut adds: a key reinstall between the stages, an invalid descriptor
 * and a buffer in the wrong state, counters of retried records, queue-full
 * back-pressure on the RRO ring. tests/up-split.c interleaves the harts.
 *
 * Every split-queue access and fence is also checked against the ordering
 * contract, per core in program order: records are stored before a fence
 * that precedes the head covering them; the consumer fences after reading a
 * head before it reads records, and after reading records before it hands
 * the slots back (tail); core 0 fences its last head before it reports idle;
 * core 1 fences its last tail before it re-enables core 0.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static int revoke_after_identity;
static int ppe_ready;

static int cur_core = 1;
static struct {
	int rec_st, rec_ld, head_st, tail_st, head_ld;
} ord[2];
static unsigned ord_checks;

static void barrier(void)
{
	__sync_synchronize();
	memset(&ord[cur_core], 0, sizeof(ord[0]));
}

static int ord_rec(volatile uint32_t *p)
{
	return (uintptr_t)p >= 0x3e902800 && (uintptr_t)p < 0x3e903000;
}

#define ORD_CTRL(word) ((volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE + 0x480) + (word))
static uint32_t ord_ld(volatile uint32_t *p)
{
	if (ord_rec(p)) {
		assert(!ord[cur_core].head_ld && "record read before fence");
		ord[cur_core].rec_ld = 1;
	}
	if (p == ORD_CTRL(0) && cur_core == 1)
		ord[1].head_ld = 1;
	return *p;
}

static void ord_st(volatile uint32_t *p, uint32_t v)
{
	if (ord_rec(p))
		ord[cur_core].rec_st = 1;
	if (p == ORD_CTRL(0)) {
		assert(!ord[cur_core].rec_st && "head before records");
		ord[cur_core].head_st = 1;
		ord_checks++;
	}
	if (p == ORD_CTRL(8)) {
		assert(!ord[cur_core].rec_ld && "tail before record reads");
		ord[cur_core].tail_st = 1;
		ord_checks++;
	}
	if (p == ORD_CTRL(1) && !v)
		assert(!ord[cur_core].head_st && "idle before last head");
	if (p == ORD_CTRL(9) && v)
		assert(!ord[cur_core].tail_st && "enable before last tail");
	*p = v;
}
#define SPLIT_LD(p) ord_ld(p)
#define SPLIT_ST(p, v) ord_st(p, v)

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

#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"
/* Tests inspect the DDR state words directly. */
#define RX_STATE_MIRROR 0
#include "../rro.c.inc"
#include "../ppe.c.inc"
/* Linux revokes the station after core 1 read its eligibility. */
#define UP_TEST_AFTER_IDENTITY_CHECK()                                        \
	do {                                                                  \
		if (revoke_after_identity)                                    \
			*(volatile uint32_t *)(uintptr_t)(AN7552_STATION_NPU + \
							  64) = 0;            \
	} while (0)
#include "../upsplit.c.inc"

static const struct {
	uint32_t address;
	size_t length;
} regions[] = {
	{ AN7552_NPU_BASE, 4096 },
	{ 0x3e900000, 0x3000 },		/* local SRAM: PPE cache, records */
	{ 0x44110000, 0xa0000 },
	{ 0x441b0000, 0x30000 },
	{ 0x442a0000, 0x10000 },
	{ 0x442c0000, 0x40000 },
	{ 0x44e00000, 0x40000 },
	{ 0x49000000, 0x30000 },
	{ 0x49200000, 4096 },
	{ 0x49300000, 4096 },
	{ 0x200d4000, 0x5000 },
	{ 0x1ec0d000, 4096 },
	{ 0x1fb50000, 4096 },
};

static struct up_front front;

static void front_call(void)
{
	cur_core = 0;
	native_up_front(&front);
	cur_core = 1;
}

static void back_call(void)
{
	cur_core = 1;
	native_up_back();
}

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

static volatile uint32_t *states(void)
{
	return rx_words(AN7552_RX_STATE_HOST);
}

static void reset(void)
{
	volatile uint32_t *q = control();

	for (unsigned int i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
		memset((void *)(uintptr_t)regions[i].address, 0, regions[i].length);
	memset(&front, 0, sizeof(front));
	revoke_after_identity = 0;
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
	for (uint32_t i = 0; i < AN7552_RX_RING_COUNT; i++) {
		volatile uint32_t *host = rx_words(0x89000000) + i * 4;

		host[0] = 0x89200000 + (i % 16) * 256;
		host[1] = 1800U << 16;
		host[2] = (1234 + i) << 16;
	}
	ppe_control()[0] = 2;
	rx_words(0x842f0000)[1] = 0x80000000;
	rx_words(0x842f0000 + 32)[1] = 0x80000000;
	pn_entry(2)[0] = 1;
	pn_entry(2)[1] = 9;
	*rx_words(AN7552_STATION_HOST + 64) = 1;
	up_init(0);
	up_init(1);
	/* Core 1's first call: RRO active, core 0 enabled. */
	back_call();
	assert(control()[5] == 1 && up_ctrl()[US_ENABLE] == 1);
}

/* Authorized CCMP TCP data, station 2, TID 5, ascending sequence/PN. */
static void packet(uint32_t token, int fragment)
{
	volatile uint32_t *p = rx_words(AN7552_RX_BUFFER_HOST + token * 2048);
	volatile uint8_t *bytes = (void *)p;
	volatile uint32_t *ring = rx_words(AN7552_RX_COMPLETE_HOST) + token * 4;

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
	states()[token] = 1;
}

/* One loop iteration of each core, core 0 first. */
static void step(void)
{
	cur_core = 0;
	up_pn_service();
	front_call();
	native_ppe_service();
	back_call();
}

static uint32_t queued(void)
{
	return up_ctrl()[US_HEAD] - up_ctrl()[US_TAIL];
}

/* Linux revokes after core 0 classified the frame for the PPE. */
static void test_revocation(void)
{
	reset();
	packet(0, 0);
	back_call();		/* start: core 1 enables core 0 */
	front_call();
	assert(queued() == 1 && (up_slot(0)[UR_CLASS] & UR_FAST_MASK));
	*rx_words(AN7552_STATION_HOST + 64) = 0;
	back_call();
	assert(!ppe_control()[1] && !ppe_control()[12]);
	assert(control()[2] == 1 && control()[7] == 1 && control()[27] == 1);
	assert(!control()[6] && !states()[0] && !queued());
	/* Revoked between the identity read and the PPE queue: the same
	 * window as the old fast recheck; core 1 never re-reads the word.
	 */
	reset();
	packet(0, 0);
	revoke_after_identity = 1;
	step();
	assert(ppe_control()[1] == 1 && control()[7] == 1);
}

static void assert_ordered_delivery(void)
{
	assert(control()[2] == 2 && control()[7] == 2);
	assert(ppe_control()[1] == 1 && ppe_control()[2] == 1);
	assert(!ppe_control()[12] && !ppe_control()[3] && !control()[6]);
	assert(rx_words(0x89200000)[12] == 1);
	assert(rx_words(0x89200100)[12] == 2);
	assert(!states()[0] && !states()[1]);
	/* Retries reuse the record's verdict, they do not re-check the PN. */
	assert((rx_words(0x842f9000)[0] & 15) == 1);
	assert((rx_words(0x842f9010)[0] & 15) == 1);
	assert(!queued());
}

static void test_ordering(int delayed, int host_full)
{
	reset();
	packet(0, 0);
	packet(1, 1);
	ppe_ready = !delayed;
	if (host_full)
		data_control()[5] = 0;
	step();
	if (delayed || host_full) {
		/* Core 0 took both descriptors; the Linux frame waits in the
		 * queue behind the one the PPE still holds.
		 */
		assert(!control()[2] && control()[16] == 2);
		assert(queued() == 1 && ppe_control()[12] == 1);
		assert((states()[0] & 255) == 2);
		assert((states()[1] & 255) == 1 && (states()[1] & 0x10000));
		step();
		assert(!control()[2] && ppe_control()[12] == 1 && queued() == 1);
		ppe_ready = 1;
		data_control()[5] = AN7552_RX_RING_COUNT - 2;
		step();
	}
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
	step();
	assert(!control()[7] && !control()[2] && queued() == 1);
	assert(control()[23] == 1);
	step();
	assert(!control()[7] && control()[23] == 1);
	rx_words(0x842f0000)[1] = 0x80000000;
	data_control()[5] = AN7552_RX_RING_COUNT - 2;
	step();
	ppe_ready = 1;
	native_ppe_service();
	assert(control()[7] == 1 && control()[2] == 1);
	assert(control()[23] == 1 && !control()[24] && !control()[6]);
	assert((rx_words(0x842f9000)[0] & 15) == 1);
	step();
	assert(control()[23] == 1);
}

static void test_continuation(void)
{
	volatile uint32_t *ring = rx_words(AN7552_RX_COMPLETE_HOST);

	reset();
	ppe_ready = 1;
	packet(0, 0);
	packet(1, 0);
	ring[1] = 160U << 16;			/* first buffer, more follows */
	rx_words(AN7552_RX_BUFFER_HOST + 2048)[12] = 0x7fffffff;
	step();
	native_ppe_service();
	assert(control()[7] == 2 && !control()[6]);
	assert(!ppe_control()[1]);
	assert(control()[2] == 2);
	assert(pn_entry(2)[4 + 5 * 2] == 1 && pn_entry(2)[5 + 5 * 2] == 0);
	assert(!control()[30]);
}

/* Station identity is cached per wcid on core 1. */
static void test_station_cache(void)
{
	volatile uint32_t *p = rx_words(AN7552_RX_BUFFER_HOST + 2048);
	volatile uint32_t *station3 = rx_words(AN7552_STATION_HOST + 3 * 32);
	volatile uint8_t *bytes = (void *)p;
	struct rx_metadata parsed;
	struct pkt_view view;

	for (int revoked = 0; revoked < 2; revoked++) {
		reset();
		packet(0, 0);
		packet(1, 0);
		p[1] = 0x800f0003;
		pn_entry(3)[0] = 1;
		pn_entry(3)[1] = 9;
		pv_init(&view, bytes, 160);
		assert(rx_parse_view(&view, 160, 0, &parsed) == RX_PARSE_OK &&
		       parsed.wcid == 3);
		for (uint32_t i = 0; i < 6; i++)
			bytes[parsed.offset + 6 + i] = 0x11 * (i + 1);
		station3[1] = 0;
		station3[2] = 0x44332211;
		station3[3] = 0x6655;
		station3[0] = !revoked;
		ppe_ready = revoked;	/* the Linux frame waits for the PPE */
		step();
		step();
		assert(control()[7] == 2 && !control()[6]);
		assert(ppe_control()[1] == 2u - revoked);
		assert(control()[27] == (uint32_t)revoked);
	}
	/* A wrong source address on the second station goes to Linux. */
	reset();
	packet(0, 0);
	packet(1, 0);
	p[1] = 0x800f0003;
	pn_entry(3)[0] = 1;
	pn_entry(3)[1] = 9;
	bytes[parsed.offset + 6] = 0x99;
	station3[2] = 0x44332211;
	station3[3] = 0x6655;
	station3[0] = 1;
	ppe_ready = 1;
	step();
	step();
	assert(ppe_control()[1] == 1 && control()[27] == 1);
	assert(control()[7] == 2 && control()[2] == 2);
}

static void pn_request(uint32_t op, uint32_t wcid, uint32_t enable)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);

	q[1] = op;
	q[2] = wcid;
	q[6] = enable;
	q[0] = q[0] + 1;
}

/* A key reinstall between the stages: the record's verdict was made under
 * the old key generation and must not reach the PPE; a plain replay check
 * (op 1) in between does not invalidate it.
 */
static void test_key_change(void)
{
	volatile uint32_t *pq = (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);

	reset();
	ppe_ready = 1;
	packet(0, 0);
	packet(1, 0);
	back_call();
	front_call();
	assert(queued() == 2);
	pq[3] = 5;
	pq[4] = 100;
	pn_request(1, 2, 0);
	up_pn_service();
	assert(pq[8] == pq[0] && !up_ctrl()[US_EPOCH]);
	pn_request(2, 2, 1);
	up_pn_service();
	assert(pq[8] == pq[0] && pq[7] == 1 && up_ctrl()[US_EPOCH] == 1);
	assert(pn_entry(2)[1] == 10 && pn_entry(2)[0] == 1);
	back_call();
	native_ppe_service();
	assert(control()[7] == 2 && !control()[6] && !queued());
	assert(!ppe_control()[1] && control()[2] == 2);
	assert(up_ctrl()[US_STALE] == 2);
	/* Epoch changed but the cookie still matches: still forwarded. */
	reset();
	ppe_ready = 1;
	packet(0, 0);
	back_call();
	front_call();
	up_ctrl()[US_EPOCH] = 7;
	back_call();
	assert(ppe_control()[1] == 1 && !up_ctrl()[US_STALE]);
}

/* Invalid descriptors stop the pipeline in order, with the old q[6] codes. */
static void test_errors(void)
{
	volatile uint32_t *ring = rx_words(AN7552_RX_COMPLETE_HOST);

	reset();
	ppe_ready = 1;
	packet(0, 0);
	packet(1, 0);
	packet(2, 0);
	ring[1 * 4] = AN7552_RX_BUFFER_HOST + 7 * 2048;	/* wrong address */
	step();
	/* Frame 0 left before the error; frame 2 was never read. */
	assert(control()[6] == 3 && control()[15] == 1 && control()[8] == 160);
	assert(control()[7] == 1 && control()[16] == 1 && front.halted);
	assert(ppe_control()[1] == 1);
	step();
	assert(control()[16] == 1 && queued() == 1);
	/* RRO stop clears it: the record is dropped, core 0 is idle. */
	control()[4] = 0;
	back_call();
	assert(control()[5] == 1);	/* core 0 still active */
	front_call();
	back_call();
	assert(!control()[5] && !queued() && up_ctrl()[US_DROPPED] == 1);
	assert(!up_ctrl()[US_ACTIVE] && !up_ctrl()[US_ENABLE]);
	/* Buffer state check on core 1. */
	reset();
	packet(0, 0);
	states()[0] = 2;
	step();
	assert(control()[6] == 3 && control()[15] == 0 && !ppe_control()[1]);
	assert(control()[23] == 1);	/* core 0 checked it before core 1 */
}

/* Counters of a record are taken once even if it is retried. */
static void test_retry_counters(void)
{
	reset();
	packet(0, 0);
	*rx_words(AN7552_STATION_HOST + 64 + 8) = 0x12345678;	/* mismatch */
	data_control()[5] = 0;					/* Linux full */
	for (int i = 0; i < 5; i++)
		step();
	assert(!control()[7] && control()[27] == 1 && control()[23] == 1);
	data_control()[5] = AN7552_RX_RING_COUNT - 2;
	step();
	assert(control()[7] == 1 && control()[2] == 1 && control()[27] == 1);
}

/* Core 0 reads no descriptor without a free record slot. */
static void test_backpressure(void)
{
	reset();
	data_control()[5] = 0;		/* Linux ring full: core 1 blocks */
	for (uint32_t t = 0; t < 100; t++)
		packet(t, 1);		/* IP fragments go to Linux */
	back_call();
	for (int i = 0; i < 10; i++) {
		front_call();
		back_call();
	}
	assert(queued() == UP_ENTRIES && control()[16] == UP_ENTRIES);
	assert(up_ctrl()[US_FULL] > 0 && up_ctrl()[US_PEAK] == UP_ENTRIES);
	assert(!control()[6] && !control()[7]);
	/* The 65th descriptor is untouched and its slot still the radio's. */
	assert(*rx_words(AN7552_RX_EMI_HOST) == UP_ENTRIES - 1);
	data_control()[5] = AN7552_RX_RING_COUNT - 2;
	for (int i = 0; i < 40 && control()[7] < 100; i++)
		step();
	assert(control()[7] == 100 && control()[16] == 100 && !queued());
	assert(control()[2] == 100 && !control()[6]);
	for (uint32_t t = 0; t < 100; t++)
		assert(!states()[t]);
}

/* A stalled record must not outlive the full key-cookie check. */
static void test_epoch_wrap(void)
{
	reset();
	packet(0, 0);
	front_call();
	assert(queued() == 1);
	uint32_t cookie = up_slot(0)[UR_COOKIE];

	for (unsigned int i = 0; i < 65536; i++) {
		pn_request(2, i ? 3 : 2, 1);
		up_pn_service();
	}
	assert(pn_entry(2)[1] != cookie);
	back_call();
	assert(!ppe_control()[1] && up_ctrl()[US_STALE] == 1);
}

/* Radio recovery recycles token 0 without restarting the NPU cores. */
static void test_dma_restart(void)
{
	reset();
	ppe_control()[0] = 0;
	packet(0, 0);
	step();
	assert(control()[2] == 1 && (rx_words(0x842f9000)[0] & 15) == 1);
	control()[4] = 0;
	back_call();
	front_call();
	back_call();
	assert(!control()[5] && !queued());
	control()[16] = control()[17] = control()[30] = control()[22] = 0;
	packet(0, 0);
	rx_words(AN7552_RX_BUFFER_HOST)[12] = 2;
	rx_words(AN7552_RX_BUFFER_HOST)[10] += 16;
	control()[4] = 1;
	back_call();
	front_call();
	back_call();
	assert((rx_words(0x842f9010)[0] & 15) == 1);
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
	test_station_cache();
	test_key_change();
	test_epoch_wrap();
	test_dma_restart();
	test_errors();
	test_retry_counters();
	test_backpressure();
	printf("ordering contract: %u head/tail stores checked\n", ord_checks);
	assert(ord_checks > 50);
	puts("PASS: upload pipeline (one thread), ordering contract on every head/tail store: revocation, ordering behind the PPE, host back-pressure, security accounting, continuation, station identity, key change between stages, error records, retry counters, queue-full back-pressure");
	return 0;
}
