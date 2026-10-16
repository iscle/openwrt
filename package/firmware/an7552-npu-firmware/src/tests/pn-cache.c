/* SPDX-License-Identifier: MIT */
/* The batch PN cache must give exactly the results and final replay table
 * of the uncached path, whatever the batch boundaries.
 */
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
#include "../pn.c.inc"
static uint32_t read32(uint32_t a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}
#include "../rx-security.c.inc"

#define FRAMES 4000
static uint32_t rng = 12345;
static uint32_t next(void)
{
	rng = rng * 1103515245 + 12345;
	return rng >> 8;
}

struct frame {
	uint8_t bytes[160];
	uint32_t batch_end;
};
static struct frame frames[FRAMES];

static void put32(uint8_t *p, uint32_t n)
{
	p[0] = n;
	p[1] = n >> 8;
	p[2] = n >> 16;
	p[3] = n >> 24;
}

static void make(void)
{
	uint32_t pn[4][4] = { { 0 } };

	for (int i = 0; i < FRAMES; i++) {
		uint8_t *b = frames[i].bytes;
		uint32_t wcid = 2 + next() % 4, tid = next() % 4, low, r;

		memset(b, 0, sizeof(frames[i].bytes));
		put32(b, 0x10000000 | 160);
		put32(b + 4, 0x800f0000 | wcid);
		put32(b + 8, 0x00040080);
		put32(b + 16, (next() % 8) < 6 ? 0 : next() % 4);
		put32(b + 40, (tid << 16) | ((next() & 0xfff) << 4));
		r = next() % 10;
		low = pn[wcid - 2][tid];
		if (r < 7)
			low += 1 + next() % 3;	/* in order, maybe gaps */
		else if (r < 9 && low)
			low -= next() % 2;	/* replay or duplicate */
		pn[wcid - 2][tid] = low > pn[wcid - 2][tid] ? low :
							     pn[wcid - 2][tid];
		put32(b + 48, low);
		b[96] = (next() % 16) ? 2 : 0xaa;	/* sometimes LLC/SNAP */
		b[97] = 0xaa;
		b[98] = 3;
		frames[i].batch_end = next() % 7 == 0;
	}
}

static uint32_t table_a[4][64], table_b[4][64];
static uint32_t result_a[FRAMES], result_b[FRAMES];
static uint32_t cookie_a[FRAMES], cookie_b[FRAMES];

static void install(void)
{
	for (uint32_t w = 2; w < 6; w++) {
		memset((void *)pn_entry(w), 0, 256);
		pn_entry(w)[0] = 1;
		pn_entry(w)[1] = 7 * w;
	}
	pn_entry(5)[0] = 0;	/* one station without a key */
}

static void rekey(uint32_t wcid)
{
	/* Core 1 PN service between batches: new generation, reset counters. */
	volatile uint32_t *e = pn_entry(wcid);

	e[0] = 0;
	for (int i = 4; i < 57; i++)
		e[i] = 0;
	e[1]++;
	e[0] = 1;
}

static void run(int cached, uint32_t *result, uint32_t *cookie,
		uint32_t table[4][64])
{
	struct pn_cache pc;
	struct pkt_view v;

	install();
	pc_reset(&pc);
	for (int i = 0; i < FRAMES; i++) {
		cookie[i] = 0;
		pv_init(&v, frames[i].bytes, 160);
		result[i] = native_rx_security_view(&v, 160, &cookie[i],
						    cached ? &pc : 0);
		if (frames[i].batch_end) {
			pc_flush(&pc);
			pc_reset(&pc);
			if (i % 997 == 0)
				rekey(2 + i % 4);
		}
	}
	pc_flush(&pc);
	for (uint32_t w = 2; w < 6; w++)
		for (int i = 0; i < 64; i++)
			table[w - 2][i] = pn_entry(w)[i];
}

int main(void)
{
	uint32_t ok = 0, replay = 0;

	assert(mmap((void *)0x44110000, 0xa0000, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)0x44110000);
	assert(mmap((void *)(uintptr_t)AN7552_NPU_BASE, 4096,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)AN7552_NPU_BASE);
	(void)native_pn_service;
	make();
	run(0, result_a, cookie_a, table_a);
	run(1, result_b, cookie_b, table_b);
	for (int i = 0; i < FRAMES; i++) {
		assert(result_a[i] == result_b[i]);
		assert(cookie_a[i] == cookie_b[i]);
		ok += (result_a[i] & 15) == 1;
		replay += (result_a[i] & 15) == 2;
	}
	assert(!memcmp(table_a, table_b, sizeof(table_a)));
	/* The sequence must exercise both accepted and rejected frames. */
	assert(ok > FRAMES / 4 && replay > FRAMES / 20);
	/* A cache without dirty words never writes. */
	{
		struct pn_cache pc;

		pc_reset(&pc);
		pc_select(&pc, 2);
		(void)pc_get(&pc, 4);
		pn_entry(2)[4] = 0x55;
		pc_flush(&pc);
		assert(pn_entry(2)[4] == 0x55);
	}
	printf("PASS: PN cache matches uncached replay checks (%u ok, %u rejected)\n",
	       ok, replay);
	return 0;
}
