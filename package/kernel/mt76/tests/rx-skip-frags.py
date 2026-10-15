#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Fault-inject the actual RX loop from a prepared (patched) mt76 dma.c.

Usage: python3 package/kernel/mt76/tests/rx-skip-frags.py /path/to/mt76/dma.c
Requires a host C compiler (CC, default cc). No kernel or hardware required.
The harness models buffer dequeue/allocation and checks that continuation
payload never reaches the frame callbacks after an allocation failure.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ENABLED(x) 0
#define MT_QFLAG_WED_RRO_EN 1
#define SKB_WITH_OVERHEAD(n) ((n) - 32)
#define native_txfree_band(q) (-1)
#define an7552_npu_txfree_read(ctx, band, field) 0
#define an7552_npu_rro_active(ctx) false
#define an7552_npu_rro_index(ctx) 0
#define mt76_queue_is_wed_rro_rxdmad_c(q) true
#define mt76_queue_is_wed_rx(q) false
#define mt76_queue_is_wed_tx_free(q) false
#define mt76_queue_is_wed_rro_ind(q) false
#define mt76_queue_is_wed_rro(q) true
#define mt76_dma_read_dma_idx(q, fallback) (fallback)
#define mt76_dma_rx_fill(dev, q, flush) ((void)0)
#define skb_mark_for_recycle(skb) ((void)0)
#define __skb_put(skb, len) ((void)0)
/* These cases never successfully allocate a non-final head. */
#define mt76_add_fragment(...) abort()

struct sk_buff {
	unsigned char *data;
	u32 cb[12];
};
struct mt76_queue {
	unsigned int flags;
	int tail, buf_size, buf_offset;
	bool rx_skip_frags;
	struct sk_buff *rx_head;
};
struct mt76_dev;
struct driver {
	void (*rx_rro_ind_process)(struct mt76_dev *, void *);
	bool (*rx_check)(struct mt76_dev *, void *, int);
	void (*rx_skb)(struct mt76_dev *, int, struct sk_buff *, u32 *);
};
struct mt76_dev {
	struct driver *drv;
	struct mt76_queue q_rx[1];
};
enum { HEAD = 1, CONTINUATION, NEXT_FRAME };
struct event {
	unsigned char bytes[256];
	bool more;
};
static struct event events[4];
static unsigned int cursor, available, allocations, delivered, freed;
static bool fail_first;

static unsigned char *
mt76_dma_dequeue(struct mt76_dev *dev, struct mt76_queue *q, bool flush,
		int *len, u32 *info, bool *more, bool *drop)
{
	(void)dev; (void)q; (void)flush; (void)info; (void)drop;
	if (cursor == available)
		return NULL;
	*more = events[cursor].more;
	*len = 100;
	return events[cursor++].bytes;
}

static struct sk_buff *napi_build_skb(void *data, int size)
{
	struct sk_buff *skb;

	(void)size;
	assert(*(unsigned char *)data != CONTINUATION);
	if (++allocations == 1 && fail_first)
		return NULL;
	skb = calloc(1, sizeof(*skb));
	assert(skb);
	skb->data = data;
	return skb;
}

static void dev_kfree_skb(struct sk_buff *skb)
{
	free(skb);
}

static void mt76_put_page_pool_buf(void *data, bool direct)
{
	(void)data; (void)direct;
	freed++;
}

static void skb_reserve(struct sk_buff *skb, int offset)
{
	skb->data += offset;
}

static bool check(struct mt76_dev *dev, void *data, int len)
{
	(void)dev; (void)len;
	assert(*(unsigned char *)data != CONTINUATION);
	return true;
}

static void receive(struct mt76_dev *dev, int queue, struct sk_buff *skb,
		    u32 *info)
{
	(void)dev; (void)queue; (void)info;
	assert(skb->data[0] != CONTINUATION);
	if (fail_first)
		assert(skb->data[0] == NEXT_FRAME);
	delivered++;
	free(skb);
}
'''

SUFFIX = r'''
static void run(bool fail, bool multi, bool split)
{
	struct driver drv = { .rx_check = check, .rx_skb = receive };
	struct mt76_dev dev = { .drv = &drv };
	struct mt76_queue *q = &dev.q_rx[0];
	unsigned int count = multi ? 4 : 2;

	memset(events, 0, sizeof(events));
	cursor = allocations = delivered = freed = 0;
	fail_first = fail;
	q->buf_size = 256;
	events[0].bytes[0] = HEAD;
	events[0].more = multi;
	if (multi) {
		events[1].bytes[0] = CONTINUATION;
		events[1].more = true;
		events[2].bytes[0] = CONTINUATION;
	}
	events[count - 1].bytes[0] = NEXT_FRAME;
	available = split ? 1 : count;
	mt76_dma_rx_process(&dev, q, 32);
	if (split) {
		assert(q->rx_skip_frags && !q->rx_head && !delivered);
		available = 2;
		mt76_dma_rx_process(&dev, q, 32);
		assert(q->rx_skip_frags && !q->rx_head && !delivered);
		available = count;
		mt76_dma_rx_process(&dev, q, 32);
	}
	assert(cursor == count);
	assert(delivered == (fail ? 1U : 2U));
	assert(freed == (fail ? count - 1 : 0));
	assert(!q->rx_skip_frags && !q->rx_head);
	assert(allocations == 2);
}

int main(void)
{
	run(false, false, false);
	run(true, false, false);
	run(true, true, false);
	run(true, true, true);
	puts("PASS: RX allocation failure, continuation skipping and poll boundaries");
	return 0;
}
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    source = Path(sys.argv[1]).read_text()
    match = re.search(
        r"static int\nmt76_dma_rx_process\([^\n]+\)\n\{.*?^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise SystemExit("Cannot locate mt76_dma_rx_process in the supplied dma.c")
    with tempfile.TemporaryDirectory(prefix="mt76-rx-test-") as tmp:
        test = Path(tmp) / "rx-skip-frags.c"
        binary = Path(tmp) / "rx-skip-frags"
        test.write_text(PREFIX + match.group() + SUFFIX)
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + ["-O2", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
