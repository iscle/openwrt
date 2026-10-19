#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inject station-handshake and PPE teardown failures into actual transport C.

These tests check fail-closed authorization and DMA-ownership decisions with
modeled MMIO. Hardware ordering still requires recovery tests on a router.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "target/linux/airoha/patches-6.18/956-net-airoha-an7552-source-npu.patch"

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define AN7552_CORES 2U
#define AN7552_STRIDE 64U
#define AN7552_MAGIC 0x7552d001U
#define AN7552_NATIVE_CONTROL 0x200U
#define AN7552_TXFREE_CONTROL 0x400U
#define AN7552_RRO_CONTROL 0x500U
#define HZ 100U
#define jiffies 1UL
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define BIT(n) (1U << (n))
#define AN7552_PPE_MODE_DRAIN 3U
#define __iomem
#define CODE_BASE 0x84000000U
#define AN7552_STATION_HOST 0x842a0000U
#define AN7552_STATION_COUNT 2048U
#define AN7552_STATION_CONTROL 0x380U
#define AN7552_STATION_AUTHORIZED 1U
#define AN7552_STATION_MLO 2U
struct an7552_npu {
	void *regs, *ram, *dev, *fe;
	bool cores_started, running, stopping, faulted, failed, fault_quiesced, ppe_enabled, tdma_enabled, pn_failed;
	u32 fault_count, ppe_saved[7], heartbeat[AN7552_CORES];
	unsigned long heartbeat_seen[AN7552_CORES];
	int station_lock;
};
static unsigned char regs[0x4000], ram[0x400000], fe[0x1000];
static unsigned int polls, failed_poll;
static bool fault_on_lock;
static int panic_timeout, halt_error;
static unsigned int stop_calls, halt_calls;
static jmp_buf panic_state;
#define WRITE_ONCE(var, value) ((var) = (value))
#define panic(...) longjmp(panic_state, 1)
static void an7552_npu_stop(struct an7552_npu *ctx) { (void)ctx; stop_calls++; }
static int halt_cores(struct an7552_npu *ctx) { (void)ctx; halt_calls++; return halt_error; }
#define spin_lock_irqsave(lock, flags) do { \
	(void)(lock); (flags) = 0; \
	if (fault_on_lock) ctx->faulted = true; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define wmb() __sync_synchronize()
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
static u32 readl(const void *p) { return *(const u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p = v; }
#define readl_poll_timeout_atomic(addr, val, cond, delay, timeout) ({ \
	u32 *ack = (addr); \
	unsigned int n = ++polls; \
	ack[0] = ack[1] = n == 1 ? 1 : 0; \
	(val) = ack[0]; \
	(n == failed_poll || !(cond)) ? -ETIMEDOUT : 0; \
})
#define readl_poll_timeout(addr, val, cond, delay, timeout) ({ \
	(void)(addr); \
	unsigned int n = ++polls; \
	(val) = 0; \
	(n == failed_poll || !(cond)) ? -ETIMEDOUT : 0; \
})
"""

CASES = r"""
int main(void)
{
	struct an7552_npu ctx = { .regs = regs, .ram = ram, .running = true };
	const u8 address[] = { 2, 3, 4, 5, 6, 7 };
	u32 *entry = (void *)(ram + AN7552_STATION_HOST - CODE_BASE + 7 * 32);
	int ret;

	/* Both initial-pause and post-publication resume failures revoke state. */
	for (unsigned int failure = 0; failure <= 2; failure++) {
		memset(regs, 0, sizeof(regs));
		memset(ram, 0, sizeof(ram));
		ctx.faulted = false;
		entry[0] = AN7552_STATION_AUTHORIZED;
		entry[7] = 1;
		polls = 0;
		failed_poll = failure;
		ret = an7552_npu_station_set(&ctx, 7, 1, 3, address, true, true, true);
		assert(ret == (failure ? -ETIMEDOUT : 0));
		if (failure) {
			assert(!entry[0] && !entry[7]);
			assert(ctx.faulted);
			/* A retry must not accept ACKs left by the timed-out command. */
			unsigned int before = polls;
			writel(1, regs + 0x3000 + AN7552_STATION_CONTROL + 4);
			writel(1, regs + 0x3000 + AN7552_STATION_CONTROL + 8);
			assert(an7552_npu_station_set(&ctx, 7, 1, 3, address,
						     true, true, true) == -EIO);
			assert(polls == before && !entry[0]);
		} else {
			assert(entry[0] == (AN7552_STATION_AUTHORIZED | AN7552_STATION_MLO));
			assert(entry[1] == (1 | (3 << 8)) && entry[4] == 0x80);
		}
		assert(!readl(regs + 0x3000 + AN7552_STATION_CONTROL));
	}
	/* Revocation is effective even when the initial pause cannot complete. */
	ctx.faulted = false;
	entry[0] = entry[7] = 1;
	polls = 0;
	failed_poll = 1;
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, NULL, false, false, false) == -ETIMEDOUT);
	assert(!entry[0] && !entry[7]);
	ctx.faulted = false;
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, NULL, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 2048, 1, 3, address, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 7, 2, 3, address, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 7, 1, 256, address, true, false, false) == -EINVAL);

	/* A caller can pass the outside-lock check before a previous owner fails. */
	ctx.faulted = false;
	fault_on_lock = true;
	polls = 0;
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, address,
				     true, true, true) == -EIO);
	assert(!polls && !entry[0]);
	fault_on_lock = false;
	entry[0] = entry[7] = 1;
	writel(1, ram + 0x110000 + 7 * 256);
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, NULL,
				     false, false, false) == -EIO);
	assert(!polls && !entry[0] && !entry[7]);
	assert(!readl(ram + 0x110000 + 7 * 256));

	/* Route/QoS policy writes share the handshake and must latch failures. */
	const u32 words[2] = { 7, 8 };
	for (unsigned int failure = 0; failure <= 2; failure++) {
		ctx.faulted = false;
		polls = 0;
		failed_poll = failure;
		ret = an7552_npu_policy_write(&ctx, entry, words, 2);
		assert(ret == (failure ? -ETIMEDOUT : 0));
		assert(ctx.faulted == !!failure);
		assert(entry[2] == !failure);
		if (failure) {
			unsigned int before = polls;
			entry[2] = 1; /* A different policy can still be eligible. */
			assert(an7552_npu_policy_write(&ctx, entry, words, 2) == -EIO);
			assert(polls == before && !entry[2]);
		}
	}

	/* Every teardown proof is mandatory before token reuse/ring restoration. */
	ctx.fe = fe;
	ctx.faulted = false;
	for (unsigned int failure = 0; failure <= 3; failure++) {
		ctx.ppe_enabled = true;
		ctx.failed = false;
		ctx.ppe_saved[0] = 0x1234;
		writel(0x9876, fe + 0x800);
		polls = 0;
		failed_poll = failure;
		native_ppe_stop(&ctx);
		assert(polls == 3);
		assert(ctx.failed == !!failure);
		assert(ctx.ppe_enabled == !!failure);
		assert(an7552_npu_recovery_ready(&ctx) == !failure);
		assert(readl(fe + 0x800) == (failure ? 0x9876U : 0x1234U));
	}

	/* Queue errors can stop traffic while both outer-loop heartbeats run. */
	const u32 queues[][3] = {
		{ AN7552_NATIVE_CONTROL, 20, 28 },
		{ AN7552_NATIVE_CONTROL + 64, 20, 28 },
		{ AN7552_TXFREE_CONTROL, 16, 24 },
		{ AN7552_TXFREE_CONTROL + 64, 16, 24 },
		{ AN7552_RRO_CONTROL, 16, 24 },
		{ 0x6c0, 0, 12 },
	};
	ctx.failed = false;
	for (unsigned int q = 0; q < sizeof(queues) / sizeof(queues[0]); q++) {
		memset(regs, 0, sizeof(regs));
		ctx.faulted = false;
		for (unsigned int core = 0; core < AN7552_CORES; core++) {
			writel(AN7552_MAGIC, regs + 0x3000 + core * AN7552_STRIDE);
			writel(1, regs + 0x3004 + core * AN7552_STRIDE);
			ctx.heartbeat[core] = 0;
		}
		writel(7, regs + 0x3000 + queues[q][0] + queues[q][2]);
		assert(an7552_npu_healthy(&ctx)); /* Disabled/stale error ignored. */
		assert(!ctx.faulted);
		writel(1, regs + 0x3000 + queues[q][0] + queues[q][1]);
		assert(!an7552_npu_healthy(&ctx));
		assert(ctx.faulted && !an7552_npu_recovery_ready(&ctx));
	}
	/* TDMA self-disables on an ownership error; it still needs recovery. */
	memset(regs, 0, sizeof(regs));
	ctx.faulted = false;
	for (unsigned int core = 0; core < AN7552_CORES; core++) {
		writel(AN7552_MAGIC, regs + 0x3000 + core * AN7552_STRIDE);
		writel(1, regs + 0x3004 + core * AN7552_STRIDE);
		ctx.heartbeat[core] = 0;
	}
	writel(1, regs + 0x3000 + AN7552_STRIDE + 56);
	assert(an7552_npu_healthy(&ctx)); /* Host never owned this TDMA. */
	ctx.tdma_enabled = true;
	assert(!an7552_npu_healthy(&ctx) && ctx.faulted);

	/* A timed-out replay mailbox cannot recover merely by heartbeating. */
	ctx.faulted = false;
	ctx.tdma_enabled = false;
	ctx.pn_failed = true;
	assert(!an7552_npu_healthy(&ctx) && ctx.faulted);
	assert(!an7552_npu_recovery_ready(&ctx));
	ctx.pn_failed = false;

	/* Only destructors invoke the fatal guard, before memory reclamation. */
	ctx.faulted = false;
	stop_calls = halt_calls = 0;
	an7552_npu_require_quiescent(NULL);
	ctx.running = false;
	an7552_npu_require_quiescent(&ctx);
	assert(!stop_calls && !halt_calls);
	ctx.running = true;
	an7552_npu_require_quiescent(&ctx);
	assert(stop_calls == 1 && halt_calls == 1);
	for (volatile unsigned int failure = 0; failure < 2; failure++) {
		ctx.failed = !failure;
		halt_error = failure ? -ETIMEDOUT : 0;
		panic_timeout = 10;
		if (!setjmp(panic_state)) {
			an7552_npu_require_quiescent(&ctx);
			assert(!"unsafe destructor returned to free DMA backing");
		}
		assert(panic_timeout == 0);
	}
	return 0;
}
"""


def transport_functions():
    patch = PATCH.read_text()
    match = re.search(
        r"\+\+\+ b/drivers/net/ethernet/airoha/an7552_npu.c\n"
        r"@@ -0,0 \+1,(\d+) @@\n", patch
    )
    if match is None:
        raise ValueError("transport source not found in patch")
    lines = patch[match.end():].splitlines(keepends=True)[:int(match[1])]
    if not all(line.startswith("+") for line in lines):
        raise ValueError("unexpected transport patch layout")
    source = "".join(line[1:] for line in lines)
    functions = []
    for name in ["an7552_npu_station_set", "an7552_npu_policy_write",
                 "native_ppe_stop", "an7552_npu_recovery_ready",
                 "an7552_npu_queue_failed", "an7552_npu_healthy",
                 "an7552_npu_require_quiescent"]:
        match = re.search(r"^(?:static )?(?:int|void|bool) " + name +
                          r"\([^;]*?\n\{.*?^\}\n", source, re.M | re.S)
        if match is None:
            raise ValueError(f"function {name} not found")
        functions.append(match[0])
    return "\n".join(functions)


class StationUpdateTest(unittest.TestCase):
    def test_timeouts_preserve_authorization_and_dma_ownership(self):
        with tempfile.TemporaryDirectory(prefix="an7552-transport-") as directory:
            source = Path(directory) / "station.c"
            binary = Path(directory) / "station"
            source.write_text(HARNESS + transport_functions() + CASES)
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) +
                ["-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
