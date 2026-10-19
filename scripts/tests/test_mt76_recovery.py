#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject the mt76 L1 reset transaction and check portable NPU stubs.

The reset test runs the production helper extracted from the patch. Hardware
DMA fencing and register behavior still require testing on a router.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "package/kernel/mt76/patches/990-an7552-source-npu.patch"
TRANSPORT = ROOT / "target/linux/airoha/patches-6.18/956-net-airoha-an7552-source-npu.patch"


def new_file(patch, name):
    section = patch.read_text().split("+++ b/" + name + "\n", 1)[1]
    section = section.split("\n--- ", 1)[0]
    return "".join(line[1:] for line in section.splitlines(keepends=True)
                   if line.startswith("+") and not line.startswith("+++"))


def compile_run(directory, source, flags=()):
    directory = Path(directory)
    c = directory / "test.c"
    exe = directory / "test"
    c.write_text(source)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", *flags, str(c), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
struct mt7996_dev { struct { int token; } mt76; };
enum { MT_MCU_CMD_RESET_DONE=1, MT_MCU_CMD_RECOVERY_DONE,
       MT_MCU_CMD_NORMAL_STATE, MT_MCU_INT_EVENT,
       MT_MCU_INT_EVENT_DMA_INIT, MT_MCU_INT_EVENT_RESET_DONE };
static unsigned int failed_wait, waits, resets, releases, idrs, commands;
static int reset_result;
static bool mt7996_wait_reset_state(struct mt7996_dev *dev, int state)
{
    assert(state == (int)++waits);
    return waits != failed_wait;
}
static int mt7996_dma_reset(struct mt7996_dev *dev, bool force)
{
    assert(!force && waits == 1);
    resets++;
    return reset_result;
}
static void mt7996_tx_token_put(struct mt7996_dev *dev)
{
    assert(resets == 1 && !reset_result);
    releases++;
}
static void idr_init(int *idr) { assert(releases == 1); idrs++; }
static void mt76_wr(struct mt7996_dev *dev, int reg, int value)
{
    assert(reg == MT_MCU_INT_EVENT && releases == 1 && idrs == 1);
    assert(value == (commands ? MT_MCU_INT_EVENT_RESET_DONE : MT_MCU_INT_EVENT_DMA_INIT));
    commands++;
}
"""

CASES = r"""
int main(void)
{
    struct mt7996_dev dev = {0};
    for (unsigned int failure = 0; failure <= 4; failure++) {
        waits = resets = releases = idrs = commands = 0;
        failed_wait = failure <= 3 ? failure : 0;
        reset_result = failure == 4 ? -EIO : 0;
        int ret = mt7996_mac_l1_reset(&dev);
        assert(ret == (failure == 4 ? -EIO : failure ? -ETIMEDOUT : 0));
        if (failure == 1 || failure == 4) {
            /* No hardware ownership proof: retain every mapping and token. */
            assert(releases == 0 && idrs == 0 && commands == 0);
        } else {
            assert(releases == 1 && idrs == 1);
            assert(commands == (failure == 2 ? 1 : 2));
        }
    }
    return 0;
}
"""


def post_image(name):
    section = PATCH.read_text().split("+++ b/" + name + "\n", 1)[1]
    section = section.split("\n--- ", 1)[0]
    return "".join(line[1:] for line in section.splitlines(keepends=True)
                   if line.startswith(("+", " ")) and not line.startswith("+++"))


class RecoveryTest(unittest.TestCase):
    def test_l1_ownership_and_handshake_failures(self):
        added = post_image("mt7996/mac.c")
        start = added.index("static int mt7996_mac_l1_reset(")
        end = added.index("\n}", start) + 3
        with tempfile.TemporaryDirectory(prefix="mt76-l1-") as directory:
            compile_run(directory, HARNESS + added[start:end] + CASES)

    def test_device_base_layout(self):
        source = post_image("mt7996/mt7996.h")
        start = source.index("union { /* must be first */")
        end = source.index("struct mt7996_phy *radio_phy", start)
        prefix = "struct mt7996_dev {\n" + source[start:end]
        with tempfile.TemporaryDirectory(prefix="mt76-layout-") as directory:
            compile_run(directory, "#include <stddef.h>\n"
                        "#define DECLARE_BITMAP(n,b) unsigned long n[17]\n"
                        "struct mt76_dev { int x; }; struct mt76_phy { int y; };\n" +
                        prefix + "};\n"
                        "_Static_assert(offsetof(struct mt7996_dev, mt76)==0, \"mt76 base\");\n"
                        "_Static_assert(offsetof(struct mt7996_dev, mphy)==0, \"phy base\");\n"
                        "int main(void) { return 0; }\n")

    def test_rekey_generation_gate(self):
        source = post_image("mt7996/main.c")
        start = source.index("static bool mt7996_npu_same_key(")
        end = source.index("\n}", source.index("static int mt7996_npu_key_gate(")) + 3
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
struct ieee80211_key_conf { int keyidx, cipher, keylen; unsigned char key[32]; };
struct mt7996_sta_link {
    struct { unsigned idx; int hw_key_idx; } wcid;
    struct ieee80211_key_conf *data_key;
};
struct mt7996_dev {
    unsigned long npu_key_used[1], npu_rx_retired[1];
    struct { struct { void *an7552; } mmio; int wq; } mt76;
    struct { bool restart; } recovery;
    int reset_work;
};
enum set_key_cmd { SET_KEY, DISABLE_KEY };
static unsigned stopped, queued;
static bool test_bit(unsigned n, unsigned long *p) { return *p & (1UL << n); }
static void set_bit(unsigned n, unsigned long *p) { *p |= 1UL << n; }
static void an7552_npu_stop(void *npu) { stopped++; }
static void queue_work(int wq, int *work) { queued++; }
"""
        cases = r"""
int main(void) {
    struct mt7996_dev d = {0};
    struct ieee80211_key_conf a = { .keyidx=0, .cipher=7, .keylen=16 }, b=a;
    struct mt7996_sta_link l = { .wcid={.idx=3, .hw_key_idx=0}, .data_key=&a };
    assert(!mt7996_npu_key_gate(&d, &l, SET_KEY, &a));
    set_bit(3, d.npu_key_used);
    assert(!mt7996_npu_key_gate(&d, &l, SET_KEY, &b)); /* same bytes, new object */
    assert(!stopped && !queued);
    b.key[0]=1;
    assert(mt7996_npu_key_gate(&d, &l, SET_KEY, &b)==-EKEYREJECTED);
    assert(stopped==1 && queued==1 && d.recovery.restart);
    assert(test_bit(3, d.npu_rx_retired));
    assert(!mt7996_npu_key_gate(&d, &l, DISABLE_KEY, &a));
    /* Removing/recreating a station must not make its WCID fresh. */
    l.data_key=NULL; l.wcid.hw_key_idx=-1;
    assert(mt7996_npu_key_gate(&d, &l, SET_KEY, &a)==-EKEYREJECTED);
    /* A failed reset retains both maps; only complete flush clears them. */
    assert(mt7996_npu_key_gate(&d, &l, SET_KEY, &b)==-EKEYREJECTED);
    d.npu_key_used[0]=d.npu_rx_retired[0]=0;
    assert(!mt7996_npu_key_gate(&d, &l, SET_KEY, &b));
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="mt76-rekey-") as directory:
            compile_run(directory, harness + source[start:end] + cases)

    def test_link_key_iterator_transaction(self):
        source = post_image("mt7996/main.c")
        start = source.index("static void\nmt7996_key_iter(")
        end = source.index("\n}", start) + 3
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#define BIT(n) (1UL<<(n))
#define IEEE80211_KEY_FLAG_PAIRWISE 1
struct ieee80211_key_conf {int flags, link_id, keyidx;};
struct ieee80211_hw {int unused;}; struct ieee80211_vif {int unused;};
struct ieee80211_sta {void *drv_priv;}; struct mt7996_sta {int unused;};
struct mt7996_sta_link {struct {int idx,hw_key_idx;} wcid; struct ieee80211_key_conf *data_key;};
struct mt7996_dev {struct {struct {void *an7552;} mmio;} mt76;unsigned long npu_rx_retired[1];};
enum set_key_cmd {SET_KEY,DISABLE_KEY};
struct mt7996_key_iter_data {int error;enum set_key_cmd cmd;unsigned link_id;struct ieee80211_sta *sta;};
static struct mt7996_dev dev={.mt76.mmio.an7552=(void*)1};
static struct mt7996_sta_link link={.wcid={.idx=2,.hw_key_idx=-1}};
static int step, fail, commits, grants, revokes, drops;
static int advance(void) {return ++step==fail?-EIO:0;}
static struct mt7996_dev *mt7996_hw_dev(struct ieee80211_hw *hw){return &dev;}
static struct mt7996_sta_link *mt7996_sta_link_protected(struct mt7996_dev *d,struct mt7996_sta *s,unsigned l){return &link;}
static int mt7996_npu_key_gate(struct mt7996_dev *d,struct mt7996_sta_link *s,enum set_key_cmd c,struct ieee80211_key_conf *k){return advance();}
static bool mt7996_npu_same_key(struct ieee80211_key_conf *a,struct ieee80211_key_conf *b){return a==b;}
static int mt7996_npu_key_links(struct mt7996_dev *d,struct ieee80211_vif *v,struct ieee80211_sta *s,unsigned long l,struct ieee80211_key_conf *k,bool enable){
 if(enable){assert(commits==1);grants++;return advance();}
 revokes++;return revokes==1?advance():0;
}
static int mt7996_npu_prepare_key(struct mt7996_dev *d,struct ieee80211_sta *s,unsigned l,enum set_key_cmd c,struct ieee80211_key_conf *k){return advance();}
static int mt7996_set_hw_key(struct ieee80211_hw *h,enum set_key_cmd c,struct ieee80211_vif *v,struct ieee80211_sta *s,unsigned l,struct ieee80211_key_conf *k){int r=advance();if(!r)link.data_key=k;return r;}
static int mt7996_npu_commit_key(struct mt7996_dev *d,struct ieee80211_sta *s,unsigned long l,struct ieee80211_key_conf *k){assert(link.data_key==k);commits++;return advance();}
static void set_bit(int b,unsigned long *p){*p|=1UL<<b;}
#define mt76_wcid_key_setup_pn(...) (drops++)
"""
        cases = r"""
int main(void){
 struct ieee80211_sta sta={0};struct ieee80211_key_conf key={.flags=1,.link_id=-1,.keyidx=0};
 for(fail=0;fail<=6;fail++){
  struct mt7996_key_iter_data it={.cmd=SET_KEY,.sta=&sta};
  step=commits=grants=revokes=drops=0;link.data_key=NULL;dev.npu_rx_retired[0]=0;
  mt7996_key_iter(NULL,NULL,&sta,&key,&it);
  assert(it.error==(fail?-EIO:0));
  if(fail){assert(link.data_key==NULL && drops==1 && dev.npu_rx_retired[0]);}
  else {assert(commits==1 && grants==1 && !drops);}
 }
 return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="mt76-keyiter-") as directory:
            compile_run(directory, harness + source[start:end] + cases, ["-Wno-sign-compare"])

    def test_portable_api_matches_transport(self):
        self.assertEqual(new_file(PATCH, "an7552_npu.h").rstrip(),
                         new_file(TRANSPORT, "include/linux/soc/airoha/an7552_npu.h").rstrip())

    def test_without_platform_kernel_header(self):
        with tempfile.TemporaryDirectory(prefix="mt76-api-") as directory:
            root = Path(directory)
            (root / "linux").mkdir()
            (root / "linux/types.h").write_text(
                "#include <stdint.h>\n#include <stdbool.h>\n"
                "typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;\n")
            (root / "linux/err.h").write_text(
                "#include <errno.h>\n#define ERR_PTR(x) ((void *)(long)(x))\n")
            (root / "an7552_npu.h").write_text(new_file(PATCH, "an7552_npu.h"))
            compile_run(directory, r'''
#include <assert.h>
#include <stddef.h>
#define IS_REACHABLE(x) 0
#include "an7552_npu.h"
int main(void)
{
    assert(!an7552_npu_active(NULL));
    assert(an7552_npu_recovery_ready(NULL));
    assert(an7552_npu_pn_check(NULL, 0, 0, NULL) == -EOPNOTSUPP);
    assert(an7552_npu_get(NULL) == ERR_PTR(-ENODEV));
    return 0;
}
''', ["-I", str(root)])


if __name__ == "__main__":
    unittest.main()
