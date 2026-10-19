#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Compile the actual mac80211 wrapper and mt7996 QoS callback against a shim.

Usage: qos-map-vlan.py /path/to/patched/mt76/mt7996/main.c
The wrapper is extracted from this tree's mac80211 patch. No kernel required.
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
#include <string.h>
typedef uint8_t u8;
#define NL80211_IFTYPE_AP 1
#define NL80211_IFTYPE_AP_VLAN 2
#define IEEE80211_MLD_MAX_NUM_LINKS 16
#define BIT(n) (1UL << (n))
#define for_each_set_bit(i, p, n) for ((i) = 0; (i) < (n); (i)++) if (*(p) & BIT(i))
#define might_sleep() ((void)0)
#define lockdep_assert_wiphy(w) ((void)(w))
#define mutex_lock(m) ((void)(m))
#define mutex_unlock(m) ((void)(m))
struct cfg80211_qos_map { int value; };
struct ieee80211_hw { void *priv, *wiphy; };
struct ieee80211_vif { int type; unsigned long valid_links; void *drv_priv; };
struct mt7996_dev { struct { struct { void *an7552; } mmio; int mutex; } mt76; };
struct mt7996_vif { u8 npu_qos_map[64]; };
struct mt7996_vif_link { struct { int idx; } mt76; };
struct ops { int (*set_qos_map)(struct ieee80211_hw *, struct ieee80211_vif *, struct cfg80211_qos_map *); };
struct ieee80211_local { struct ieee80211_hw hw; struct ops *ops; };
struct ieee80211_sub_if_data { struct ieee80211_local *local; struct ieee80211_vif vif; bool in_driver; };
static int checked, commands;
#define trace_drv_set_qos_map(...) ((void)0)
#define trace_drv_return_int(...) ((void)0)
static bool check_sdata_in_driver(struct ieee80211_sub_if_data *s) { checked++; return s->in_driver; }
static struct mt7996_dev *mt7996_hw_dev(struct ieee80211_hw *h) { return h->priv; }
static int mt7996_npu_build_qos_map(u8 *m, struct cfg80211_qos_map *q) { memset(m, q ? q->value : 0, 64); return 0; }
static struct mt7996_vif_link *mt7996_vif_link(struct mt7996_dev *d, struct ieee80211_vif *v, unsigned int i) {
 static struct mt7996_vif_link l; (void)d; (void)v; (void)i; return &l;
}
static int mt7996_mcu_set_qos_map(struct mt7996_dev *d, int b, const u8 *m) { (void)d; (void)b; (void)m; commands++; return 0; }
static int an7552_npu_qos_map(void *d, int b, const u8 *m) { (void)d; (void)b; (void)m; commands++; return 0; }
'''
SUFFIX = r'''
int main(void) {
 struct mt7996_dev dev = {};
 struct ops ops = { .set_qos_map = mt7996_set_qos_map };
 struct ieee80211_local local = { .hw = { .priv = &dev }, .ops = &ops };
 struct ieee80211_sub_if_data vlan = { .local = &local, .vif = { .type = NL80211_IFTYPE_AP_VLAN, .drv_priv = (void *)1 } };
 struct cfg80211_qos_map requested = { .value = 5 };
 /* VLAN has no add_interface(), no usable driver-private state. */
 assert(!drv_set_qos_map(&local, &vlan, &requested));
 assert(!checked && !commands);
 assert(!drv_set_qos_map(&local, &vlan, NULL));
 dev.mt76.mmio.an7552 = &dev;
 assert(drv_set_qos_map(&local, &vlan, &requested) == -EOPNOTSUPP);
 assert(drv_set_qos_map(&local, &vlan, NULL) == -EOPNOTSUPP);
 assert(!checked && !commands); /* Native policy is never partially changed. */
 vlan.vif.type = NL80211_IFTYPE_AP;
 assert(drv_set_qos_map(&local, &vlan, &requested) == -EIO);
 assert(checked == 1 && !commands);
 struct mt7996_vif mvif = {};
 vlan.vif.drv_priv = &mvif;
 vlan.in_driver = true;
 assert(!drv_set_qos_map(&local, &vlan, &requested));
 assert(commands == 1 && mvif.npu_qos_map[0] == 5);
 ops.set_qos_map = NULL;
 vlan.vif.type = NL80211_IFTYPE_AP_VLAN;
 assert(!drv_set_qos_map(&local, &vlan, &requested));
 puts("PASS: software VLAN acceptance, native refusal, interface lifetime and ordinary AP maps");
}
'''


def extract(source, name):
    m = re.search(r"(?:static )?int\s+" + name + r"\([^;]+?\n\{.*?^\}", source, re.M | re.S)
    if not m:
        raise SystemExit(f"Cannot locate {name}")
    return m.group()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    patch = Path(__file__).resolve().parents[2] / "mac80211/patches/subsys/394-mac80211-propagate-qos-map-to-offload.patch"
    additions = "\n".join(line[1:] for line in patch.read_text().splitlines() if line.startswith("+") and not line.startswith("+++"))
    body = extract(Path(sys.argv[1]).read_text(), "mt7996_set_qos_map") + "\n" + extract(additions, "drv_set_qos_map")
    with tempfile.TemporaryDirectory(prefix="mt76-qos-test-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(PREFIX + body + SUFFIX)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    main()
