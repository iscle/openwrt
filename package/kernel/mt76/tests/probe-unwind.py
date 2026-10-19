#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Fault-inject the actual registration/cleanup functions from patched mt76.

Usage: python3 probe-unwind.py /path/to/mt76/mt7996/init.c
This host harness verifies staged unwind and producer-before-memory ordering.
It does not emulate DMA or replace on-device allocation/IRQ fault injection.
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
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MT76_STATE_INITIALIZED 0
#define MT76_STATE_REGISTERED 1
#define MT_BAND1 1
#define MT_BAND2 2
#define MT7996_TOKEN_SIZE 8192
#define WRITE_ONCE(a, b) ((a) = (b))
#define INIT_WORK(...) ((void)0)
#define INIT_DELAYED_WORK(...) ((void)0)
#define INIT_LIST_HEAD(...) ((void)0)
#define init_waitqueue_head(...) ((void)0)
#define mutex_init(...) ((void)0)
struct ieee80211_hw { int unused; };
struct napi { bool dev, enabled; };
struct mphy { unsigned long state; void *priv; };
struct mt7996_dev;
struct mt76_queue { int ndesc; void *page_pool; };
struct mt76_phy { struct mt76_queue *q_tx[2]; };
struct mt7996_phy { struct mt7996_dev *dev; struct mphy *mt76; };
struct hif { int irq; };
struct mt76_dev {
 void *dev;
 struct mphy phy;
 struct { void *an7552; int wed, wed_hif2; } mmio;
 struct mt76_phy *phys[3];
 struct mt76_queue *q_mcu[2], q_rx[4];
 int tx_worker;
 void *napi_dev, *tx_napi_dev;
 struct napi napi[4], tx_napi;
 bool tx_napi_disabled;
 int token_start, token_size, irq_tasklet;
};
struct mt7996_dev {
 struct mt76_dev mt76;
 struct mphy mphy;
 struct mt7996_phy phy;
 struct { bool shutdown, hw_init_done; } recovery;
 unsigned long recovery_napi_disabled;
 int init_work, dump_work, reset_work, rc_work;
 struct { int work; } wed_rro;
 struct hif *hif2;
 int irq;
};
static struct ieee80211_hw test_hw;
static int mt76_rates[1];
static int fail_stage, stage, cleanups, irq_sync, killed, disabled, stopped;
static int napi_deleted, rx_cleaned;
static int unregistered, quiesced, poll_tasklet_pending, polls_exited, irq_freed, device_freed;
static struct mt7996_dev *current;
#define mt76_hw(d) (&test_hw)
#define mt7996_for_each_phy(d, p) for ((p) = &(d)->phy; (p); (p) = NULL)
#define mt7996_phy2(d) NULL
#define mt7996_phy3(d) NULL
static bool test_bit(int n, const unsigned long *p) { return !!(*p & (1UL << n)); }
static void clear_bit(int n, unsigned long *p) { *p &= ~(1UL << n); }
static int step(void) { return stage++ == fail_stage ? -12 : 0; }
static bool an7552_npu_active(void *p) { return !!p; }
static int mt7996_init_hardware(struct mt7996_dev *d) {
 if (fail_stage == -2) return -12;
 d->mphy.state |= 1UL << MT76_STATE_INITIALIZED;
 /* Some NAPI instances do not exist until the later RRO stage. */
 d->mt76.napi[0] = (struct napi){ true, true };
 d->mt76.q_rx[0].ndesc = d->mt76.q_rx[2].ndesc = 8;
 d->mt76.tx_napi = (struct napi){ true, true };
 return step();
}
static void mt7996_init_wiphy(struct ieee80211_hw *h, int *w) { (void)h; (void)w; }
static int mt7996_register_phy(struct mt7996_dev *d, int b) { (void)d; (void)b; return step(); }
static int mt7996_npu_hw_init(struct mt7996_dev *d) { (void)d; return step(); }
static int mt7996_dma_rro_start(struct mt7996_dev *d) {
 d->mt76.napi[2] = (struct napi){ true, true };
 return step();
}
static int mt76_register_device(struct mt76_dev *d, bool b, int *r, size_t n) {
 int ret = step(); (void)b; (void)r; (void)n;
 if (!ret) d->phy.state |= 1UL << MT76_STATE_REGISTERED;
 return ret;
}
static void mt7996_thermal_init(struct mt7996_phy *p) { (void)p; }
static void ieee80211_queue_work(struct ieee80211_hw *h, int *w) { (void)h; (void)w; }
static int mt7996_init_debugfs(struct mt7996_dev *d) { (void)d; return step(); }
static int mt7996_coredump_register(struct mt7996_dev *d) { (void)d; return step(); }
static void cancel_work_sync(int *w) { (void)w; }
static void an7552_npu_require_quiescent(void *p) { (void)p; quiesced++; }
static void mt7996_unregister_phy(struct mt7996_phy *p) { (void)p; }
static void mt7996_unregister_thermal(struct mt7996_phy *p) { (void)p; }
static void mt7996_coredump_unregister(struct mt7996_dev *d) { (void)d; }
static void mt76_unregister_device(struct mt76_dev *d) {
 if (test_bit(MT76_STATE_REGISTERED, &d->phy.state)) unregistered++;
}
static void mt7996_irq_disable(struct mt7996_dev *d, unsigned int m) {
 assert(!test_bit(MT76_STATE_INITIALIZED, &d->mphy.state));
 (void)m;
}
static void synchronize_irq(int irq) { assert(irq == 7 || irq == 8); irq_sync++; }
static void tasklet_kill(int *t) {
 (void)t; assert(irq_sync == 2);
 for (size_t i = 0; i < ARRAY_SIZE(current->mt76.napi); i++)
  assert(!current->mt76.napi[i].enabled);
 assert(!current->mt76.tx_napi.enabled && polls_exited);
 poll_tasklet_pending = 0; killed++;
}
static void synchronize_net(void) {
 assert(!current->mt76.tx_napi.enabled);
 /* Complete a poll which relinquished NAPI ownership before returning. */
 poll_tasklet_pending = 1;
 polls_exited = 1;
}
static void mt7996_npu_hw_stop(struct mt7996_dev *d) { (void)d; }
static void mt76_npu_stop(struct mt76_dev *d) { (void)d; }
static void an7552_npu_stop(void *d) { (void)d; }
static void napi_disable(struct napi *n) {
 assert(irq_sync == 2 && n->dev && n->enabled);
 /* A racing final poll can schedule irq_tasklet despite INITIALIZED=0. */
 poll_tasklet_pending = 1;
 n->enabled = false; disabled++;
}
static void mt7996_mcu_exit(struct mt7996_dev *d) { (void)d; }
static void mt7996_dma_disable(struct mt7996_dev *d, bool reset) {
 (void)d; assert(reset && killed && quiesced); stopped++;
}
static void mt7996_tx_token_put(struct mt7996_dev *d) { (void)d; assert(stopped); }
#define mt76_for_each_q_rx(d, i) for ((i) = 0; (i) < 4; (i)++) if ((d)->q_rx[i].ndesc)
#define EXPORT_SYMBOL_GPL(...)
static void mt76_worker_disable(int *w) { (void)w; }
static void netif_napi_del(struct napi *n) {
 assert(n->dev && !n->enabled); n->dev = false; napi_deleted++;
}
static void mt76_dma_tx_cleanup(struct mt76_dev *d, struct mt76_queue *q, bool force) {
 (void)d; (void)q; assert(force);
}
static void mt76_dma_rx_cleanup(struct mt76_dev *d, struct mt76_queue *q) {
 (void)d; assert(q->ndesc); rx_cleaned++;
}
static void page_pool_destroy(void *p) { (void)p; }
static bool mtk_wed_device_active(int *w) { return !!*w; }
static void mtk_wed_device_detach(int *w) { (void)w; }
static void mt76_free_pending_txwi(struct mt76_dev *d) { (void)d; }
static void mt76_free_pending_rxwi(struct mt76_dev *d) { (void)d; }
static void free_netdev(void *p) { (void)p; }
void mt76_dma_cleanup(struct mt76_dev *dev);
static void mt7996_dma_cleanup(struct mt7996_dev *d) {
 for (size_t i = 0; i < ARRAY_SIZE(d->mt76.napi); i++) assert(!d->mt76.napi[i].enabled);
 assert(stopped && quiesced && !poll_tasklet_pending);
 mt76_dma_cleanup(&d->mt76); cleanups++;
}
static void mt7996_wed_rro_free(struct mt7996_dev *d) { (void)d; assert(cleanups); }
struct pci_dev { struct mt76_dev *data; };
#define pci_get_drvdata(p) ((p)->data)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
static void devm_free_irq(void *d, int irq, void *context) {
 (void)d; (void)context; assert(irq == 7 || irq == 8); assert(cleanups); irq_freed++;
}
static void mt7996_put_hif2(struct hif *h) { (void)h; assert(irq_freed == 2); }
static void mt76_free_device(struct mt76_dev *d) { (void)d; assert(irq_freed == 2); device_freed++; }
static void mt7996_cleanup_device(struct mt7996_dev *dev);
'''
SUFFIX = r'''
int main(void) {
 for (fail_stage = -2; fail_stage < 8; fail_stage++) {
  struct hif companion = { .irq = 8 };
  struct mt7996_dev d = { .irq = 7, .hif2 = &companion };
  stage = cleanups = irq_sync = killed = disabled = stopped = unregistered = quiesced = 0;
  current = &d;
  poll_tasklet_pending = polls_exited = irq_freed = device_freed = 0;
  napi_deleted = rx_cleaned = 0;
  d.mt76.mmio.an7552 = &d;
  int ret = mt7996_register_device(&d);
  if (fail_stage == -2) { assert(ret && !cleanups && !irq_sync); continue; }
  if (fail_stage == -1) {
   assert(!ret && !cleanups);
   struct pci_dev pci = { .data = &d.mt76 };
   mt7996_pci_remove(&pci);
   assert(device_freed == 1);
  } else assert(ret);
  assert(cleanups == 1 && killed == 1 && irq_sync == 2);
  assert(rx_cleaned == 2);
  assert(napi_deleted == (fail_stage < 0 || fail_stage >= 4 ? 3 : 2));
  assert(d.recovery.shutdown && !d.recovery.hw_init_done);
  assert(unregistered == (fail_stage < 0 || fail_stage >= 6));
  if (fail_stage != -1) {
   assert(!device_freed); /* Error caller still owns the IRQ/device lifetime. */
   mt7996_cleanup_device(&d);
   assert(cleanups == 1); /* The initialized-stage guard is one-shot. */
  }
 }
 puts("PASS: every post-DMA probe stage, pre-registration safety, IRQ/NAPI ordering");
}
'''

def extract(source, name):
    m = re.search(r"(?:static )?(?:int|void)\s+" + name + r"\([^;]+?\n\{.*?^\}", source, re.M | re.S)
    if not m:
        raise SystemExit(f"Cannot locate {name}")
    return m.group()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    source = Path(sys.argv[1]).read_text()
    body = extract(source, "mt7996_register_device") + "\n" + extract(source, "mt7996_cleanup_device")
    body += "\n" + extract(source, "mt7996_unregister_device")
    body += "\n" + extract(Path(sys.argv[1]).with_name("pci.c").read_text(), "mt7996_pci_remove")
    body += "\n" + extract(Path(sys.argv[1]).parent.parent.joinpath("dma.c").read_text(), "mt76_dma_cleanup")
    with tempfile.TemporaryDirectory(prefix="mt76-probe-test-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(PREFIX + body + SUFFIX)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    main()
