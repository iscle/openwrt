#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual transport station-update function with modeled MMIO.

This checks authorization on both handshake timeout paths. It does not model
hardware ordering or substitute for DMA/recovery tests on a router.
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
typedef uint32_t u32;
typedef uint8_t u8;
#define __iomem
#define CODE_BASE 0x84000000U
#define AN7552_STATION_HOST 0x842a0000U
#define AN7552_STATION_COUNT 2048U
#define AN7552_STATION_CONTROL 0x380U
#define AN7552_STATION_AUTHORIZED 1U
#define AN7552_STATION_MLO 2U
struct an7552_npu {
	void *regs, *ram, *dev;
	bool running, stopping, faulted;
	int station_lock;
};
static unsigned char regs[0x4000], ram[0x400000];
static unsigned int polls, failed_poll;
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define wmb() __sync_synchronize()
#define dev_err(...) ((void)0)
static u32 readl(const void *p) { return *(const u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p = v; }
#define readl_poll_timeout_atomic(addr, val, cond, delay, timeout) ({ \
	u32 *ack = (addr); \
	unsigned int n = ++polls; \
	ack[0] = ack[1] = n == 1 ? 1 : 0; \
	(val) = ack[0]; \
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
		entry[0] = AN7552_STATION_AUTHORIZED;
		entry[7] = 1;
		polls = 0;
		failed_poll = failure;
		ret = an7552_npu_station_set(&ctx, 7, 1, 3, address, true, true, true);
		assert(ret == (failure ? -ETIMEDOUT : 0));
		if (failure) {
			assert(!entry[0] && !entry[7]);
		} else {
			assert(entry[0] == (AN7552_STATION_AUTHORIZED | AN7552_STATION_MLO));
			assert(entry[1] == (1 | (3 << 8)) && entry[4] == 0x80);
		}
		assert(!readl(regs + 0x3000 + AN7552_STATION_CONTROL));
	}
	/* Revocation is effective even when the initial pause cannot complete. */
	entry[0] = entry[7] = 1;
	polls = 0;
	failed_poll = 1;
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, NULL, false, false, false) == -ETIMEDOUT);
	assert(!entry[0] && !entry[7]);
	assert(an7552_npu_station_set(&ctx, 7, 1, 3, NULL, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 2048, 1, 3, address, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 7, 2, 3, address, true, false, false) == -EINVAL);
	assert(an7552_npu_station_set(&ctx, 7, 1, 256, address, true, false, false) == -EINVAL);
	return 0;
}
"""


def station_function():
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
    start = source.index("int an7552_npu_station_set(")
    end = source.index("EXPORT_SYMBOL_GPL(an7552_npu_station_set)", start)
    return source[start:end]


class StationUpdateTest(unittest.TestCase):
    def test_handshake_errors_revoke_authorization(self):
        with tempfile.TemporaryDirectory(prefix="an7552-transport-") as directory:
            source = Path(directory) / "station.c"
            binary = Path(directory) / "station"
            source.write_text(HARNESS + station_function() + CASES)
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) +
                ["-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
