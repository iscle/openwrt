/* SPDX-License-Identifier: MIT */
/* Optional cycle profile (make PROF=1, off by default). Core 0 publishes at
 * NPU_BASE+0x280, core 1 at NPU_BASE+0x2c0, 16 words each, visible in the
 * debugfs status rows 280: and 2c0:. Linux never touches 0x280-0x2ff after
 * the boot memset.
 *
 *   [0] all loop cycles >> 8
 *   [1]..[4] cycles >> 8 of the profiled sections, see firmware_main
 *       (core 0: txfree, transmit, download;
 *        core 1: -, ppe_service, up_back)
 *   [5] loop iterations (raw count)
 *   [6] busy iterations (raw count): the activity marker changed
 *   [7] cycles >> 8 of busy iterations
 *   [8] activity counter snapshot taken with the same publish
 *       (core 0: forward[10], core 1: RRO q[7])
 *   [9], [10] cycles >> 8 of sections 5 and 6
 *       (core 0: up_front, pn service)
 *   [15] PROF_MAGIC
 *
 * The core 0 activity marker also counts the upload records it produced
 * (US_HEAD), so front-half iterations count as busy.
 *
 * Sums are kept in 64-bit locals and published every 256 iterations, so the
 * 32-bit words wrap after 2^40 cycles (~23 min at 800 MHz); use modular
 * differences between two samples.
 */
#ifndef AN7552_PROF_H
#define AN7552_PROF_H

#ifdef NPU_PROF
#define PROF_MAGIC 0x9f0f0002U
#define PROF_BASE 0x280U
#define PROF_SLOTS 8U

struct prof {
	uint64_t sum[PROF_SLOTS];
	uint32_t last, iterations, busy, marker;
};

static inline uint32_t prof_cycle(void)
{
	uint32_t c;

	__asm__ volatile("csrr %0, mcycle" : "=r"(c) : : "memory");
	return c;
}

static inline volatile uint32_t *prof_slots(uint32_t core)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + PROF_BASE + core * 64);
}

static uint32_t prof_marker(uint32_t core)
{
	if (core)
		return read32(AN7552_NPU_BASE + AN7552_RRO_CONTROL + 7 * 4);
	return read32(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL + 10 * 4) +
	       read32(AN7552_NPU_BASE + AN7552_TXFREE_CONTROL + 7 * 4) +
	       read32(AN7552_NPU_BASE + AN7552_TXFREE_CONTROL + 64 + 7 * 4) +
	       up_ctrl()[US_HEAD];
}

static void prof_init(struct prof *p, uint32_t core)
{
	for (uint32_t i = 0; i < PROF_SLOTS; i++)
		p->sum[i] = 0;
	p->iterations = 0;
	p->busy = 0;
	p->marker = prof_marker(core);
	prof_slots(core)[15] = PROF_MAGIC;
	p->last = prof_cycle();
}

/* Called at the top of every loop iteration: accounts the previous one. */
static void prof_tick(struct prof *p, uint32_t core)
{
	uint32_t now = prof_cycle(), delta = now - p->last, marker;
	volatile uint32_t *out;

	p->last = now;
	p->sum[0] += delta;
	p->iterations++;
	marker = prof_marker(core);
	if (marker != p->marker) {
		p->busy++;
		p->sum[7] += delta;
		p->marker = marker;
	}
	if (p->iterations & 255)
		return;
	out = prof_slots(core);
	for (uint32_t i = 0; i < 5; i++)
		out[i] = (uint32_t)(p->sum[i] >> 8);
	out[5] = p->iterations;
	out[6] = p->busy;
	out[7] = (uint32_t)(p->sum[7] >> 8);
	out[8] = core ? marker :
			read32(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL + 10 * 4);
	out[9] = (uint32_t)(p->sum[5] >> 8);
	out[10] = (uint32_t)(p->sum[6] >> 8);
}

#define PROF_DECLARE(p, core) struct prof p; prof_init(&p, core)
#define PROF_TICK(p, core) prof_tick(&p, core)
#define PROF(p, slot, ...)                                                   \
	do {                                                                 \
		uint32_t prof_t_ = prof_cycle();                             \
		__VA_ARGS__;                                                 \
		(p).sum[slot] += prof_cycle() - prof_t_;                     \
	} while (0)
#else
#define PROF_DECLARE(p, core) do { } while (0)
#define PROF_TICK(p, core) do { } while (0)
#define PROF(p, slot, ...) do { __VA_ARGS__; } while (0)
#endif

#endif
