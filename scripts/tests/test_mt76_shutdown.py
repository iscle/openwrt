#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual PCI shutdown/link code with injected ownership failure."""

from pathlib import Path
import tempfile
import unittest
from test_mt76_recovery import compile_run

ROOT = Path(__file__).resolve().parents[2]
PATCH = next((ROOT / "package/kernel/mt76/patches").glob("993-*.patch"))


def function(name):
    section = PATCH.read_text().split("+++ b/mt7996/pci.c\n", 1)[1]
    source = "".join(line[1:] for line in section.splitlines(keepends=True)
                     if line.startswith((" ", "+")) and not line.startswith("+++"))
    start = source.index(name + "(")
    end = source.index("\n}", start) + 3
    return source[start:end]


class ShutdownTest(unittest.TestCase):
    def test_companion_link_direction_and_failure(self):
        harness = r"""
#include <assert.h>
#include <errno.h>
#include <stddef.h>
struct device {int id;};
struct pci_dev {struct device dev;};
struct mt7996_hif {struct device *dev;};
#define DL_FLAG_AUTOREMOVE_CONSUMER 4
static struct device *expected_consumer,*expected_supplier;
static int calls,fail;
static void *device_link_add(struct device *consumer,struct device *supplier,int flags){
 assert(consumer==expected_consumer && supplier==expected_supplier);
 assert(flags==DL_FLAG_AUTOREMOVE_CONSUMER);calls++;return fail?NULL:(void*)1;
}
"""
        cases = r"""
int main(void){struct pci_dev p={0};struct device h={0};struct mt7996_hif hif={.dev=&h};
 expected_consumer=&p.dev;expected_supplier=&h;
 assert(!mt7996_pci_link_hif2(&p,NULL) && !calls);
 assert(!mt7996_pci_link_hif2(&p,&hif) && calls==1);
 fail=1;assert(mt7996_pci_link_hif2(&p,&hif)==-ENOMEM && calls==2);
 return 0;}
"""
        with tempfile.TemporaryDirectory(prefix="mt76-link-") as directory:
            compile_run(directory, harness + "static int " + function("mt7996_pci_link_hif2") + cases)

    def test_shutdown_drains_before_radio_reset(self):
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
struct mt76_dev {struct {void *an7552;} mmio;};
struct mt7996_dev {struct mt76_dev mt76;struct {bool shutdown;} recovery;int dump_work,reset_work;};
struct pci_dev {struct mt76_dev *data;};
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define WRITE_ONCE(x,v) ((x)=(v))
static struct mt7996_dev d;
static int events,fail;
static jmp_buf escape;
static struct mt76_dev *pci_get_drvdata(struct pci_dev *p){return p->data;}
static void cancel_work_sync(int *work){
 assert(d.recovery.shutdown);
 assert(work==(events==0?&d.dump_work:&d.reset_work));assert(events<2);events++;
}
static void an7552_npu_shutdown(void *ctx){assert(events++==2);if(fail)longjmp(escape,1);}
static void mt7996_wfsys_reset(struct mt7996_dev *dev){assert(events++==3);}
static void pci_clear_master(struct pci_dev *p){assert(!p->data || events==4);events++;}
"""
        cases = r"""
int main(void){struct pci_dev p={.data=&d.mt76};
 mt7996_pci_shutdown(&p);assert(events==5 && d.recovery.shutdown);
 events=0;fail=1;d.recovery.shutdown=false;
 if(!setjmp(escape)){mt7996_pci_shutdown(&p);assert(0);}
 assert(events==3 && d.recovery.shutdown); /* fence failure: no radio reset/clear-master */
 p.data=NULL;events=0;mt7996_pci_shutdown(&p);assert(events==1);
 return 0;}
"""
        with tempfile.TemporaryDirectory(prefix="mt76-shutdown-") as directory:
            compile_run(directory, harness + "static void " + function("mt7996_pci_shutdown") + cases)


if __name__ == "__main__":
    unittest.main()
