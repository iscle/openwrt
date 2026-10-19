/* SPDX-License-Identifier: MIT */
/* Upload pipeline across the two harts, whole receive path.
 *
 * Core 0 (pn service + native_up_front), core 1 (PPE service +
 * native_up_back) and a host actor (radio, PPE, Linux) run as threads; a
 * seeded scheduler lets exactly one run between two shared accesses, so every
 * run is deterministic. Stores of the harts to the record queue and its
 * control words go through per-hart store buffers that commit in random
 * order (biased to newest first) except across barrier().
 *
 * The radio fills buffers posted by core 1's refill and writes completion
 * descriptors (phase bits, EMI-limited space). The PPE consumes queued
 * descriptors, forwards most frames (release) and returns some to Linux.
 * Linux consumes its ring in order and stalls now and then.
 *
 * Checked:
 *   every frame leaves exactly once; forwarded frames and Linux frames are
 *     each in radio order; a buffer is never reused while the pipeline, the
 *     PPE or a queued record still refers to it;
 *   queue full: core 0 stops reading the RRO ring (US_FULL), nothing lost;
 *   a forwarding verdict always matches the current key generation and the
 *     station table (key reinstall and station change with full queue);
 *   RRO stop with a full queue: q[5]=0 only with core 0 idle and the queue
 *     empty, nothing published afterwards; restart shows no stale record;
 *   at rest: free list + posted buffers account for all 6144 tokens.
 */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

enum { CORE0, CORE1, HOST, ACTORS };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn = HOST;
static int alive[ACTORS] = { 1, 1, 1 };
static unsigned weight[ACTORS] = { 4, 4, 3 };
static __thread int self = HOST;
static uint64_t rng = 0x7552a0b1c2d3ULL;
static int stop_cores;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 11);
}

/* Per-hart store buffers for the split queue words. */
#define SB 64
struct pending {
	volatile uint32_t *p;
	uint32_t v;
};
static struct pending sb[2][SB];
static int sbn[2];

static void commit(int c, int i)
{
	/* Stores to one word stay in program order. */
	for (int j = 0; j < i; j++)
		if (sb[c][j].p == sb[c][i].p) {
			i = j;
			break;
		}
	*sb[c][i].p = sb[c][i].v;
	memmove(&sb[c][i], &sb[c][i + 1], (sbn[c] - i - 1) * sizeof(sb[c][0]));
	sbn[c]--;
}

static void drain(int c)
{
	while (sbn[c])
		commit(c, 0);
}

static void yield(void)
{
	unsigned total = 0, r;
	int next;

	/* Commit one buffered store, often the newest: a flag stored after
	 * its data without a fence then becomes visible first.
	 */
	for (int c = 0; c < 2; c++)
		if (sbn[c] && !(rnd() % 3))
			commit(c, rnd() & 1 ? sbn[c] - 1 : (int)(rnd() % sbn[c]));
	for (int a = 0; a < ACTORS; a++)
		total += alive[a] ? weight[a] : 0;
	if (!total)
		return;
	r = rnd() % total;
	for (next = 0; next < ACTORS; next++) {
		unsigned w = alive[next] ? weight[next] : 0;

		if (r < w)
			break;
		r -= w;
	}
	if (next == self)
		return;
	turn = next;
	pthread_cond_broadcast(&cv);
	while (turn != self)
		pthread_cond_wait(&cv, &mu);
}

static uint32_t view(int c, volatile uint32_t *p)
{
	for (int i = sbn[c] - 1; i >= 0; i--)
		if (sb[c][i].p == p)
			return sb[c][i].v;
	return *p;
}

static uint32_t split_ld(volatile uint32_t *p)
{
	yield();
	return self < 2 ? view(self, p) : *p;
}

static void split_st(volatile uint32_t *p, uint32_t v)
{
	yield();
	if (self >= 2) {
		*p = v;
		return;
	}
	if (sbn[self] == SB)
		commit(self, 0);
	sb[self][sbn[self]].p = p;
	sb[self][sbn[self]].v = v;
	sbn[self]++;
}

static void barrier(void)
{
	yield();
	if (self < 2)
		drain(self);
}

/* PPE return registers: fe4 count, fe0 status, fdc metadata; the firmware
 * acknowledges the front entry by writing 0x80000000 to fe0, which the
 * model notices at its next register read.
 */
#define FIFO 256
static uint32_t fifo_st[FIFO], fifo_md[FIFO], fifo_r, fifo_w;
static int presented;

static void ppe_ack_poll(void)
{
	volatile uint32_t *fe0 = (void *)(uintptr_t)0x1fb50fe0;

	if (*fe0 != 0x80000000)
		return;
	assert(self == CORE1 && presented && fifo_w != fifo_r);
	fifo_r++;
	presented = 0;
	*fe0 = 0;
}

static uint32_t read32(uint32_t address)
{
	yield();
	if (address == 0x1fb50fe4 || address == 0x1fb50fe0 ||
	    address == 0x1fb50fdc)
		ppe_ack_poll();
	if (address == 0x1fb50fe4)
		return (fifo_w - fifo_r) & 65535;
	if (address == 0x1fb50fe0) {
		if (fifo_w == fifo_r)
			return 0;
		presented = 1;
		return fifo_st[fifo_r % FIFO];
	}
	if (address == 0x1fb50fdc) {
		assert(presented);
		return fifo_md[fifo_r % FIFO];
	}
	return *(volatile uint32_t *)(uintptr_t)address;
}

#define SPLIT_LD(p) split_ld(p)
#define SPLIT_ST(p, v) split_st(p, v)

static void check_forward(uint32_t fast, uint32_t wcid,
			  volatile uint32_t *rec);
#define UP_TEST_AFTER_RECHECK(fast, wcid, rec) check_forward(fast, wcid, rec)

#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"
#include "../rro.c.inc"
#include "../ppe.c.inc"
#include "../upsplit.c.inc"

#define N AN7552_RX_RING_COUNT
#define H 128U			/* Linux rings (host data, completion) */
#define HOSTBUF 0x8a000000U
#define MAXID 65536
#define WCID 2U

static volatile uint32_t *rro(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
}

static volatile uint32_t *ppe_q(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
}

static volatile uint32_t *data_q(uint32_t band)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				   band * 64);
}

static volatile uint32_t *station(void)
{
	return (void *)(uintptr_t)(AN7552_STATION_NPU + WCID * 32);
}

static volatile uint32_t *station_ctl(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_STATION_CONTROL);
}

/* Committed state of a split control word. */
static uint32_t mem(uint32_t word)
{
	return up_ctrl()[word];
}

/* ---- bookkeeping */
static uint8_t seen_fwd[MAXID], seen_linux[MAXID], dropped[MAXID];
static uint32_t generated, last_fwd = ~0U, last_linux = ~0U, n_fwd, n_linux;
static uint32_t session_first;		/* first id of the current RRO run */
static uint32_t last_id[AN7552_RX_BUFFER_COUNT];
static uint32_t forward_checks;
static uint32_t cur_mac_low = 0x33221102, cur_mac_high = 0x5544;

static void check_forward(uint32_t fast, uint32_t wcid, volatile uint32_t *rec)
{
	volatile uint32_t *st =
		(void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);

	if (!fast)
		return;
	/* Validated under the current key generation, from this station. */
	assert(pn_entry(wcid)[1] == rec[UR_COOKIE]);
	assert(st[2] == rec[UR_MAC_LOW] &&
	       st[3] == (rec[UR_MAC_HIGH] & 65535));
	forward_checks++;
}

static void finished(uint32_t id)
{
	assert(id < generated && id >= session_first);
	assert(!seen_fwd[id] && !seen_linux[id] && !dropped[id]);
}

/* ---- radio */
static uint32_t rc[2], cw, cphase, to_send, pn_next = 1;
static int radio_on;

static void radio_frame(uint32_t band)
{
	volatile uint32_t *d = data_q(band);
	volatile uint32_t *slot, *p, *c;
	volatile uint8_t *bytes;
	uint32_t filled = *(volatile uint32_t *)(uintptr_t)(d[4] + 8);
	uint32_t emi = *rx_words(AN7552_RX_EMI_HOST), token, id;

	if ((filled + 1 + N - rc[band]) % N == 0)
		return;				/* nothing posted */
	if ((cw + N - (emi + 1) % N) % N >= N - 1)
		return;				/* completion ring full */
	slot = rx_words(AN7552_RX_DATA_RING_HOST + band * 0x8000 + rc[band] * 16);
	token = (slot[2] >> 16) - AN7552_RX_TOKEN_BASE;
	assert(token < AN7552_RX_BUFFER_COUNT);
	assert(slot[0] == AN7552_RX_BUFFER_HOST + token * 2048);
	/* Posted by refill (or by Linux before core 1 loaded the states). */
	assert((rro()[5] ? st_get(token) :
			   rx_words(AN7552_RX_STATE_HOST)[token]) ==
	       (1 | (band << 8)));
	/* The previous frame in this buffer is completely gone. */
	if (last_id[token] != ~0U)
		assert(seen_fwd[last_id[token]] || seen_linux[last_id[token]] ||
		       dropped[last_id[token]]);
	id = generated++;
	assert(id < MAXID);
	last_id[token] = id;
	p = rx_words(AN7552_RX_BUFFER_HOST + token * 2048);
	bytes = (void *)p;
	memset((void *)p, 0, 160);
	p[0] = 0x10000000 | 160;
	p[1] = 0x800f0000 | WCID;
	p[2] = 0x00040080;
	p[3] = 1U << 16;
	p[8] = 0x4188;
	p[10] = (5U << 16) | ((id & 0xfff) << 4);
	/* Every 13th frame replays an old PN: the replay check rejects it. */
	p[12] = id % 13 == 6 ? 1 : pn_next++;
	p[21] = id;
	bytes[96] = 2;
	for (uint32_t i = 0; i < 4; i++)
		bytes[102 + i] = cur_mac_low >> (i * 8);
	bytes[106] = cur_mac_high & 255;
	bytes[107] = cur_mac_high >> 8;
	bytes[108] = 8;
	bytes[110] = 0x45;
	bytes[113] = 50;
	bytes[119] = id % 7 == 3 ? 1 : 6;	/* ICMP goes to Linux */
	bytes[142] = 0x50;
	c = rx_words(AN7552_RX_COMPLETE_HOST + cw * 16);
	c[0] = AN7552_RX_BUFFER_HOST + token * 2048;
	c[1] = (160U << 16) | (1U << 30);
	c[2] = (AN7552_RX_TOKEN_BASE + token) << 16;
	c[3] = cphase << 28;
	if (++cw == N) {
		cw = 0;
		cphase = (cphase + 1) & 15;
	}
	rc[band] = (rc[band] + 1) % N;
	*(volatile uint32_t *)(uintptr_t)(d[4] + 12) = rc[band];
}

/* ---- PPE */
static uint32_t hc;
static int ppe_stall;

static void ppe_step(void)
{
	volatile uint32_t *doorbell = (void *)(uintptr_t)0x1fb50808;

	for (uint32_t n = rnd() % 4; n && !ppe_stall && hc != *doorbell &&
				     fifo_w - fifo_r < FIFO;
	     n--) {
		volatile uint32_t *d = rx_words(0x842f0000 + hc * 32);
		uint32_t token = (d[4] >> 14) & 0xffff, id, release;

		assert(!(d[1] & 0x80000000) && token < AN7552_RX_BUFFER_COUNT);
		assert((st_get(token) & 255) == 2);
		id = rx_words(AN7552_RX_BUFFER_HOST + token * 2048)[21];
		release = rnd() % 11 != 5;
		if (release) {
			finished(id);
			assert(last_fwd == ~0U || id > last_fwd);
			last_fwd = id;
			seen_fwd[id] = 1;
			n_fwd++;
		}
		fifo_st[fifo_w % FIFO] =
			0x80000000 | (release ? 0x40000000 : 0) | token;
		fifo_md[fifo_w % FIFO] = id & 0x1fffff;
		fifo_w++;
		d[1] |= 0x80000000;
		hc = (hc + 1) % 64;
		*(volatile uint32_t *)(uintptr_t)0x1fb5080c = hc;
	}
}

/* ---- Linux */
static uint32_t lcons, hcons[2];
static int linux_stall, toggle_stall;

static void linux_step(void)
{
	volatile uint32_t *q = rro();

	if (toggle_stall && !(rnd() % 50))
		linux_stall = !linux_stall;
	while (!linux_stall && lcons != q[2]) {
		volatile uint32_t *c = rx_words(q[0] + lcons * 16), *host;
		uint32_t address = c[0], band, entry, id;

		assert(address >= HOSTBUF && address < HOSTBUF + 2 * H * 2048);
		band = (address - HOSTBUF) / (H * 2048);
		entry = (address - HOSTBUF) % (H * 2048) / 2048;
		assert(entry == hcons[band]);
		id = rx_words(address)[21];
		finished(id);
		assert(last_linux == ~0U || id > last_linux);
		last_linux = id;
		seen_linux[id] = 1;
		n_linux++;
		host = rx_words(data_q(band)[0] + entry * 16);
		host[1] = 2048U << 16;
		hcons[band] = (hcons[band] + 1) % H;
		data_q(band)[5] = (hcons[band] + H - 2) % H;
		lcons = (lcons + 1) % H;
		*rx_words(q[18]) = (lcons + H - 2) % H;
	}
}

static void host_step(void)
{
	if (radio_on && to_send)
		for (uint32_t n = rnd() % 4; n && to_send; n--) {
			uint32_t before = generated;

			radio_frame(rnd() & 1);
			to_send -= generated - before;
		}
	ppe_step();
	linux_step();
	yield();
}

static void run(unsigned steps)
{
	while (steps--)
		host_step();
}

static void set_weights(unsigned c0, unsigned c1, unsigned host)
{
	weight[CORE0] = c0;
	weight[CORE1] = c1;
	weight[HOST] = host;
}

static void wait_for(volatile uint32_t *word, uint32_t value, unsigned limit)
{
	unsigned n = 0;

	while (*word != value && n++ < limit)
		host_step();
	assert(*word == value);
}

/* ---- Linux setup (an7552_npu_rro_setup / native_ppe_start) */
static void ppe_start(void)
{
	volatile uint32_t *q = ppe_q();

	for (uint32_t i = 0; i < 16; i++)
		q[i] = 0;
	for (uint32_t i = 0; i < 64; i++) {
		rx_words(0x842f0000 + i * 32)[1] = 0x80000000;
		rx_words(0x842f0000 + i * 32)[5] = 0x7f4087ff;
	}
	q[10] = hc;
	*(volatile uint32_t *)(uintptr_t)0x1fb50808 = hc;
	q[0] = 2;
}

static void rro_setup(void)
{
	volatile uint32_t *q = rro();
	uint32_t token = 0;

	for (uint32_t i = 0; i < 32; i++)
		q[i] = 0;
	for (uint32_t i = 0; i < AN7552_RX_BUFFER_COUNT; i++) {
		rx_words(AN7552_RX_STATE_HOST)[i] = 0;
		rx_words(AN7552_RX_FREE_HOST)[i] = i;
		last_id[i] = ~0U;
	}
	for (uint32_t band = 0; band < 2; band++) {
		volatile uint32_t *d = data_q(band);
		uint32_t regs = 0x200d4500 + band * 0x4000;

		for (uint32_t i = 0; i < 16; i++)
			d[i] = 0;
		for (uint32_t i = 0; i < N - 1; i++, token++) {
			volatile uint32_t *slot = rx_words(
				AN7552_RX_DATA_RING_HOST + band * 0x8000 + i * 16);

			slot[0] = AN7552_RX_BUFFER_HOST + token * 2048;
			slot[1] = (2048U << 16) | (1U << 8);
			slot[2] = (AN7552_RX_TOKEN_BASE + token) << 16;
			rx_words(AN7552_RX_STATE_HOST)[token] = 1 | (band << 8);
		}
		d[0] = 0x89000000 + band * 0x8000;
		d[1] = H;
		d[4] = regs;
		d[5] = H - 2;
		d[6] = N - 1;
		d[11] = 2048;
		*(volatile uint32_t *)(uintptr_t)(regs + 8) = N - 2;
		*(volatile uint32_t *)(uintptr_t)(regs + 12) = 0;
		rc[band] = 0;
		hcons[band] = 0;
		for (uint32_t i = 0; i < H; i++) {
			volatile uint32_t *host =
				rx_words(d[0] + i * 16);

			host[0] = HOSTBUF + (band * H + i) * 2048;
			host[1] = 2048U << 16;
			host[2] = i << 16;
		}
	}
	for (uint32_t i = 0; i < N; i++) {
		volatile uint32_t *c = rx_words(AN7552_RX_COMPLETE_HOST + i * 16);

		c[0] = c[1] = c[2] = 0;
		c[3] = 15U << 28;
	}
	/* Model convention: EMI = last consumed index. */
	*rx_words(AN7552_RX_EMI_HOST) = N - 1;
	cw = 0;
	cphase = 0;
	lcons = 0;
	q[0] = 0x89010000;
	q[1] = H;
	q[18] = 0x89300000;
	*rx_words(q[18]) = H - 2;
	q[20] = token;
	q[21] = 0;
	q[22] = AN7552_RX_BUFFER_COUNT - token;
	ppe_start();
	session_first = generated;
	q[4] = 1;
}

/* an7552_npu_station_set(), authorized, under the two-core quiesce. */
static void station_set(uint32_t mac_low, uint32_t mac_high)
{
	volatile uint32_t *ctl = station_ctl(), *st = station();
	unsigned n = 0;

	ctl[0] = 1;
	while (!(ctl[1] == 1 && ctl[2] == 1) && n++ < 200000)
		host_step();
	assert(ctl[1] == 1 && ctl[2] == 1);
	st[0] = 0;
	st[4] = 0;
	st[1] = 0;
	st[2] = mac_low;
	st[3] = mac_high;
	st[0] = 1;
	cur_mac_low = mac_low;
	cur_mac_high = mac_high;
	ctl[0] = 0;
	n = 0;
	while ((ctl[1] || ctl[2]) && n++ < 200000)
		host_step();
	assert(!ctl[1] && !ctl[2]);
}

static uint32_t pn_sequence;

/* pn_request(): op 2 installs a key, resets counters, bumps word 1. */
static void pn_request(uint32_t op, uint32_t enable)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);

	q[1] = op;
	q[2] = WCID;
	q[6] = enable;
	q[0] = ++pn_sequence;
	wait_for(&q[8], pn_sequence, 200000);
	assert(q[7] == 1);
}

/* ---- the harts */
static struct up_front front;

static void core0_step(void)
{
	volatile uint32_t *ctl = station_ctl();

	up_pn_service();
	native_up_front(&front);
	if (ctl[0]) {
		barrier();
		ctl[1] = 1;
		return;
	}
	ctl[1] = 0;
}

static void core1_step(void)
{
	volatile uint32_t *ctl = station_ctl();

	native_ppe_service();
	if (ctl[0]) {
		barrier();
		ctl[2] = 1;
		return;
	}
	ctl[2] = 0;
	native_up_back();
}

static void *core_main(void *arg)
{
	self = (int)(intptr_t)arg;
	pthread_mutex_lock(&mu);
	while (turn != self)
		pthread_cond_wait(&cv, &mu);
	up_init(self);
	while (!stop_cores) {
		if (self == CORE0)
			core0_step();
		else
			core1_step();
		yield();
	}
	drain(self);
	alive[self] = 0;
	turn = HOST;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	return NULL;
}

static pthread_t threads[2];

static void start_cores(void)
{
	alive[CORE0] = alive[CORE1] = 1;
	stop_cores = 0;
	for (int c = 0; c < 2; c++)
		assert(!pthread_create(&threads[c], NULL, core_main,
				       (void *)(intptr_t)c));
}

static void join_cores(void)
{
	stop_cores = 1;
	while (alive[CORE0] || alive[CORE1])
		yield();
	pthread_mutex_unlock(&mu);
	for (int c = 0; c < 2; c++)
		pthread_join(threads[c], NULL);
	pthread_mutex_lock(&mu);
}

/* Everything handed out: radio idle, Linux and PPE drained, queue empty. */
static int all_done(void)
{
	for (uint32_t id = session_first; id < generated; id++)
		if (!seen_fwd[id] && !seen_linux[id] && !dropped[id])
			return 0;
	return mem(US_HEAD) == mem(US_TAIL) &&
	       view(CORE0, &up_ctrl()[US_HEAD]) == mem(US_HEAD) &&
	       !ppe_q()[12] && fifo_r == fifo_w;
}

static void settle(void)
{
	radio_on = 0;
	linux_stall = 0;
	toggle_stall = 0;
	ppe_stall = 0;
	for (unsigned n = 0; !all_done(); n++) {
		assert(n < 4000);
		run(200);
	}
	assert(!rro()[6] && !ppe_q()[3]);
}

/* Buffer conservation at rest. */
static void check_buffers(void)
{
	volatile uint32_t *q = rro();
	uint32_t count[4] = { 0 }, posted = 0;
	static uint8_t in_free[AN7552_RX_BUFFER_COUNT];

	run(2000);		/* let refill catch up and states settle */
	for (uint32_t t = 0; t < AN7552_RX_BUFFER_COUNT; t++) {
		uint32_t s = st_get(t);

		assert(!(s & 0x10000) && (s & 255) < 2);
		count[s & 255]++;
		in_free[t] = 0;
	}
	for (uint32_t band = 0; band < 2; band++)
		posted += (data_q(band)[6] + N - rc[band]) % N;
	assert(count[1] == posted);
	assert(count[0] == q[22] && count[0] + count[1] == AN7552_RX_BUFFER_COUNT);
	for (uint32_t i = 0; i < q[22]; i++) {
		uint32_t t = rx_words(AN7552_RX_FREE_HOST)[(q[20] + i) %
							  AN7552_RX_BUFFER_COUNT];

		assert(t < AN7552_RX_BUFFER_COUNT && !in_free[t] && !st_get(t));
		in_free[t] = 1;
	}
}

/* A: traffic at varying core speeds with Linux stalls. */
static void test_traffic(void)
{
	static const unsigned w[][3] = {
		{ 4, 4, 3 }, { 6, 1, 3 }, { 1, 6, 3 }, { 6, 0, 3 },
		{ 0, 6, 3 }, { 3, 3, 1 },
	};
	uint32_t start = generated;

	radio_on = 1;
	to_send = 6000;
	toggle_stall = 1;
	for (unsigned round = 0; to_send || round < 12; round++) {
		assert(round < 600);
		set_weights(w[round % 6][0], w[round % 6][1], w[round % 6][2]);
		run(3000);
		assert(!rro()[6] && !ppe_q()[3]);
	}
	set_weights(4, 4, 3);
	settle();
	check_buffers();
	assert(mem(US_FULL) > 0 && up_ctrl()[US_PEAK] == UP_ENTRIES);
	assert(!mem(US_DROPPED) && rro()[7] == generated - start);
	printf("A: %u frames, %u forwarded, %u to Linux, %u full-queue stops, peak %u\n",
	       generated - start, n_fwd, n_linux, mem(US_FULL),
	       up_ctrl()[US_PEAK]);
}

/* Fill the queue behind a stalled PPE. */
static void fill_queue(void)
{
	unsigned n = 0;

	radio_on = 1;
	to_send = 400;
	ppe_stall = 1;
	while (view(CORE0, &up_ctrl()[US_HEAD]) - mem(US_TAIL) < UP_ENTRIES &&
	       n++ < 2000)
		run(100);
	assert(view(CORE0, &up_ctrl()[US_HEAD]) - mem(US_TAIL) == UP_ENTRIES);
}

/* B: key reinstall while 64 validated records wait. */
static void test_key_change(void)
{
	uint32_t stale = up_ctrl()[US_STALE], checks = forward_checks;

	fill_queue();
	pn_request(2, 1);
	ppe_stall = 0;
	settle();
	check_buffers();
	assert(up_ctrl()[US_STALE] >= stale + UP_ENTRIES / 2);
	assert(forward_checks > checks);
	printf("B: key reinstall with a full queue, %u stale verdicts sent to Linux\n",
	       up_ctrl()[US_STALE] - stale);
}

/* C: station address change while records with the old one wait. */
static void test_station_change(void)
{
	uint32_t mismatch = rro()[27], checks = forward_checks;

	fill_queue();
	station_set(0x77665544, 0x9988);
	ppe_stall = 0;
	radio_on = 1;
	to_send = 300;
	while (to_send)
		run(500);
	settle();
	check_buffers();
	assert(rro()[27] >= mismatch + UP_ENTRIES / 2);
	assert(forward_checks > checks + 100);
	printf("C: station change with a full queue, %u old-address frames sent to Linux\n",
	       rro()[27] - mismatch);
}

/* D: RRO stop with a full queue, then Linux setup and restart. */
static void test_stop_restart(void)
{
	volatile uint32_t *q = rro();
	uint32_t head, drops = mem(US_DROPPED), lost = 0, first;

	fill_queue();
	q[4] = 0;
	wait_for(&q[5], 0, 400000);
	/* q[5]=0: core 0 idle, queue empty, nothing published afterwards. */
	assert(!mem(US_ACTIVE) && !mem(US_ENABLE));
	head = mem(US_HEAD);
	assert(head == mem(US_TAIL) && view(CORE0, &up_ctrl()[US_HEAD]) == head);
	assert(mem(US_DROPPED) - drops >= UP_ENTRIES);
	run(5000);
	assert(mem(US_HEAD) == head && mem(US_TAIL) == head && !q[5]);
	/* native_ppe_stop(): drain releases everything. */
	ppe_stall = 0;
	ppe_q()[0] = 3;
	wait_for(&ppe_q()[12], 0, 400000);
	ppe_q()[0] = 0;
	wait_for(&ppe_q()[13], 0, 400000);
	radio_on = 0;
	for (uint32_t id = session_first; id < generated; id++)
		if (!seen_fwd[id] && !seen_linux[id]) {
			dropped[id] = 1;
			lost++;
		}
	first = generated;
	rro_setup();
	radio_on = 1;
	to_send = 2000;
	toggle_stall = 1;
	while (to_send)
		run(1000);
	settle();
	check_buffers();
	for (uint32_t id = 0; id < first; id++)
		assert(seen_fwd[id] || seen_linux[id] || dropped[id]);
	assert(last_fwd >= first && last_linux >= first);
	printf("D: RRO stop with a full queue (%u records dropped, %u frames lost), restart, 2000 frames, no stale record\n",
	       mem(US_DROPPED) - drops, lost);
}

static void map(uint32_t address, size_t length)
{
	assert(mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)address);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	map(AN7552_NPU_BASE, 4096);
	map(0x3e900000, 0x3000);
	map(0x44110000, 0xa0000);
	map(0x441b0000, 0x30000);
	map(0x442a0000, 0x10000);
	map(0x442c0000, 0x40000);
	map(0x44e00000, 0xc00000);
	map(0x49000000, 0x30000);
	map(0x49300000, 4096);
	map((HOSTBUF & 0x3fffffff) | 0x40000000, 2 * H * 2048);
	map(0x200d4000, 0x5000);
	map(0x1ec0d000, 4096);
	map(0x1fb50000, 4096);
	(void)native_pn_service;
	pthread_mutex_lock(&mu);

	/* Linux boot: shared words cleared, station, key, RRO, PPE set up,
	 * then the cores are released.
	 */
	memset((void *)(uintptr_t)AN7552_NPU_BASE, 0, 0x800);
	station()[1] = 0;
	station()[2] = cur_mac_low;
	station()[3] = cur_mac_high;
	station()[0] = 1;
	pn_entry(WCID)[0] = 1;
	pn_entry(WCID)[1] = 9;
	rro_setup();
	start_cores();
	while (mem(US_MAGIC) != UP_MAGIC)
		yield();

	test_traffic();
	test_key_change();
	test_station_change();
	test_stop_restart();

	join_cores();
	pthread_mutex_unlock(&mu);
	printf("PASS: upload pipeline across two harts: exactly-once in-order delivery, queue-full back-pressure, key and station changes with a full queue, RRO stop/restart, buffer conservation (%u frames, %u forward checks)\n",
	       generated, forward_checks);
	return 0;
}
