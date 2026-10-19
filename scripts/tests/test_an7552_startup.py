#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real boot/attachment failure paths with modeled NPU hardware.

A failed readiness check is distinct from proof that the cores stopped.
These tests do not establish physical reset or interconnect fence semantics.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_be5000_platform import ROOT, function
from test_an7552_shutdown import final_source

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <setjmp.h>
#include "protocol.h"
#include "firmware-abi.h"
typedef uint32_t u32;
#define __iomem
#define CODE_BASE 0x84000000U
#define BOOT_CONFIG 0x306004
#define BOOT_TRIGGER 0x306000
#define BOOT_PC(core) (0x306020 + (core) * 4)
#define PN_TABLE_HOST 0x84110000U
#define PN_DOMAIN_HOST AN7552_PN_DOMAIN_HOST
#define PN_RADIO_STATE_HOST AN7552_PN_RADIO_STATE_HOST
#define EPROBE_DEFER 517
#define THIS_MODULE 0
#define DL_FLAG_AUTOREMOVE_CONSUMER 1
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define wmb() __sync_synchronize()
#define usleep_range(...) ((void)0)
#define mutex_lock(...) ((void)0)
#define mutex_unlock(...) ((void)0)
#define device_lock(...) ((void)0)
#define device_unlock(...) ((void)0)
#define of_node_put(...) ((void)0)
#define platform_device_put(...) ((void)0)
#define try_module_get(...) true
#define module_put(...) ((void)0)
#define device_link_add(...) true
#define panic(...) longjmp(panic_state, 1)
struct device_node {int unused;};
struct device {void *of_node, *driver;};
struct platform_driver {int driver;};
static struct platform_driver an7552_npu_driver;
struct an7552_npu {
    void *regs, *ram;
    struct device *dev;
    int attach_lock;
    bool running, cores_started, failed, stopping, faulted, fault_quiesced;
    bool pn_failed, pn_domains_initialized, shutdown, attached;
    u32 heartbeat[2], heartbeat_seen[2], pn_sequence;
    int queue_error;
};
struct platform_device {struct device dev;struct an7552_npu *ctx;};
struct firmware {size_t size;const uint8_t *data;};
static unsigned char regs[0x310200], ram[0x400000];
static u32 fw_words[16];
static struct firmware fw_image={sizeof(fw_words),(void *)fw_words};
static struct an7552_npu ctx;
static struct platform_device pdev;
static struct device_node node;
static struct device consumer;
static bool hardware_live;
static int boot_mode, park_error, tdma_error, parks, copies, triggers, requests;
static int panic_timeout;
static jmp_buf panic_state;
#define jiffies 1
static struct device_node *of_parse_phandle(void *n,const char *s,int i){return &node;}
static bool of_device_is_compatible(struct device_node *n,const char *s){return true;}
static struct platform_device *of_find_device_by_node(struct device_node *n){return &pdev;}
static struct an7552_npu *platform_get_drvdata(struct platform_device *p){return p->ctx;}
static int request_firmware(const struct firmware **f,const char *s,struct device *d)
{ requests++;*f=&fw_image;return 0; }
static void release_firmware(const struct firmware *f){}
static u32 get_unaligned_le32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static u32 readl(const void *p)
{
    uintptr_t offset=(const unsigned char *)p-regs;
    if(hardware_live && (offset==0x3004 || offset==0x3044) && boot_mode!=2)
        ++*(u32 *)p;
    return *(const u32 *)p;
}
static void writel(u32 v,void *p)
{
    *(u32 *)p=v;
    if(p==regs+BOOT_TRIGGER && v==1) {
        assert(ctx.cores_started);hardware_live=true;triggers++;
        for(unsigned i=0;i<2;i++) {
            *(u32 *)(regs+0x3000+i*AN7552_STRIDE)=
                boot_mode==1 && i==1 ? 0 : AN7552_MAGIC;
            *(u32 *)(regs+0x3000+AN7552_FW_RUNTIME_ABI+i*4)=
                AN7552_FW_ABI_VERSION;
        }
    }
}
#define readl_poll_timeout(addr,val,condition,delay,timeout) ({ \
    (val)=readl(addr);(condition)?0:-ETIMEDOUT; \
})
static void memcpy_toio(void *dst,const void *src,size_t n)
{ assert(!hardware_live);copies++;memcpy(dst,src,n); }
static void memset_io(void *dst,int v,size_t n)
{ assert(!hardware_live);memset(dst,v,n); }
static u32 recovery_timer_stop(struct an7552_npu *c){return 0;}
static int park_cores(struct an7552_npu *c)
{
    assert(c->cores_started || c->running);parks++;
    if(!park_error)hardware_live=false;
    return park_error;
}
static void __an7552_npu_stop(struct an7552_npu *c){}
static int tdma_start(struct an7552_npu *c){return tdma_error;}
'''

CASES = r'''
static void reset(void)
{
    memset(&ctx,0,sizeof(ctx));memset(regs,0,sizeof(regs));memset(ram,0,sizeof(ram));
    memset(fw_words,0,sizeof(fw_words));
    fw_words[0]=AN7552_FW_MAGIC;fw_words[1]=AN7552_FW_FORMAT_VERSION;
    fw_words[2]=AN7552_FW_ABI_VERSION;fw_words[3]=AN7552_FW_HEADER_SIZE;
    fw_words[4]=sizeof(fw_words);fw_words[5]=AN7552_FW_HEADER_SIZE;
    fw_words[6]=AN7552_CORES;
    ctx.regs=regs;ctx.ram=ram;ctx.dev=&pdev.dev;
    pdev.dev.driver=&an7552_npu_driver.driver;pdev.ctx=&ctx;
    hardware_live=false;boot_mode=park_error=tdma_error=parks=copies=triggers=requests=0;
}
static void must_panic(void)
{
    panic_timeout=10;
    if(!setjmp(panic_state)) {
        __an7552_npu_require_quiescent(&ctx);
        assert(!"unproved startup returned to destructor");
    }
    assert(!panic_timeout && ctx.cores_started);
}
int main(void)
{
    /* Invalid images never release a core or create a shutdown obligation. */
    reset();fw_words[0]=0;
    assert(boot_firmware(&ctx)==-EINVAL);
    assert(!ctx.cores_started && !triggers && !parks && !copies);

    for(int mode=1;mode<=2;mode++) {
        /* Either a missing core or a stalled heartbeat must be parked, even
         * though successful runtime initialization was never published. */
        reset();boot_mode=mode;
        assert(boot_firmware(&ctx)==-ETIMEDOUT);
        assert(parks==1 && !ctx.cores_started && !ctx.running && !hardware_live);
        assert(!ctx.failed);
        boot_mode=0;
        assert(!boot_firmware(&ctx));
        assert(ctx.running && ctx.cores_started && copies==2);
        assert(!halt_cores(&ctx) && !ctx.cores_started && !ctx.running);

        reset();boot_mode=mode;park_error=-ETIMEDOUT;
        assert(boot_firmware(&ctx)==-ETIMEDOUT);
        assert(ctx.failed && ctx.cores_started && !ctx.running && hardware_live);
        assert(parks==1 && copies==1 && requests==1);
        /* Both direct retry and consumer reattachment retain quarantine. */
        assert(boot_firmware(&ctx)==-EIO);
        assert(an7552_npu_get(&consumer)==ERR_PTR(-EIO));
        assert(copies==1 && requests==1 && triggers==1);
        must_panic();
    }

    /* Recovery must park a previously ready image before replacing its RAM. */
    reset();
    assert(!boot_firmware(&ctx) && hardware_live);
    assert(!boot_firmware(&ctx));
    assert(parks==1 && copies==2 && ctx.running && ctx.cores_started);
    assert(!halt_cores(&ctx));

    /* A successful firmware boot followed by TDMA failure has the same
     * obligation. Attachment cleanup must not erase a failed halt. */
    reset();tdma_error=-EBUSY;park_error=-ETIMEDOUT;
    assert(an7552_npu_get(&consumer)==ERR_PTR(-EBUSY));
    assert(ctx.failed && ctx.cores_started && ctx.running && !ctx.attached);
    assert(parks==1 && copies==1 && hardware_live);
    assert(an7552_npu_get(&consumer)==ERR_PTR(-EIO));
    assert(copies==1 && triggers==1);
    must_panic();

    reset();tdma_error=-EBUSY;
    assert(an7552_npu_get(&consumer)==ERR_PTR(-EBUSY));
    assert(!ctx.failed && !ctx.cores_started && !ctx.running && parks==1);
    tdma_error=0;
    assert(an7552_npu_get(&consumer)==&ctx);
    assert(ctx.attached && ctx.running && ctx.cores_started && copies==2);
    __an7552_npu_require_quiescent(&ctx);
    assert(!ctx.cores_started && !ctx.running && !hardware_live);
}
'''


class StartupTest(unittest.TestCase):
    def test_failed_startup_retains_quiescence_obligation(self):
        with tempfile.TemporaryDirectory(prefix='an7552-startup-') as tmp:
            source = final_source(tmp)
            signatures = (
                'static bool __an7552_npu_recovery_ready(',
                'static int halt_cores(',
                'static void __an7552_npu_require_quiescent(',
                'static int boot_firmware(',
                'struct an7552_npu *an7552_npu_get(',
            )
            code = HARNESS + '\n'.join(function(source[source.rindex(s):], s)
                                        for s in signatures) + CASES
            cfile, binary = Path(tmp)/'test.c', Path(tmp)/'test'
            cfile.write_text(code)
            result = subprocess.run(shlex.split(os.getenv('HOSTCC', 'cc')) +
                                    ['-std=gnu11', '-O2', '-Wall',
                                     '-I'+str(ROOT/'package/firmware/an7552-npu-firmware/src'),
                                     str(cfile), '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
