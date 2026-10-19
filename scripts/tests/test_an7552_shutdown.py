#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the patched NPU terminal gate with concurrent real API functions.

Only hardware operations are modeled. This proves host admission/lifetime
ordering, not device DMA draining or successful warm reboot under load.
"""
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_be5000_platform import ROOT, function

PATCHES = ROOT / 'target/linux/airoha/patches-6.18'


def final_source(tmp):
    name = 'drivers/net/ethernet/airoha/an7552_npu.c'
    path = Path(tmp) / name
    path.parent.mkdir(parents=True)
    # This patch creates the file; later diffs modify it in place.
    source = (PATCHES / '956-net-airoha-an7552-source-npu.patch').read_text()
    section = source.split('+++ b/' + name + '\n', 1)[1].split('\n--- ', 1)[0]
    path.write_text('\n'.join(line[1:] for line in section.splitlines()
                              if line.startswith('+')) + '\n')
    for patch in ('958-net-airoha-quiesce-dma-on-shutdown.patch',
                  '962-net-airoha-block-NPU-restarts-during-shutdown.patch'):
        text = (PATCHES / patch).read_text()
        section = text.split('--- a/' + name + '\n', 1)[1].split('\n--- a/', 1)[0].rstrip('\n') + '\n'
        lines = path.read_text().splitlines(True)
        position = 0
        for hunk in re.split(r'^@@[^\n]*\n', section, flags=re.M)[1:]:
            old = [line[1:] for line in hunk.splitlines(True) if line[:1] in (' ', '-')]
            new = [line[1:] for line in hunk.splitlines(True) if line[:1] in (' ', '+')]
            matches = [i for i in range(position, len(lines)-len(old)+1)
                       if lines[i:i+len(old)] == old]
            if len(matches) != 1:
                raise AssertionError(f'{patch}: non-unique exact patch context {matches}: {old[:3]!r} ... {old[-3:]!r}')
            start = matches[0]
            lines[start:start+len(old)] = new
            position = start + len(new)
        path.write_text(''.join(lines))
    return path.read_text()


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define __iomem
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p,v) __atomic_store_n((p),(v), __ATOMIC_RELEASE)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define spin_lock_irqsave(l,f) do {(f)=0;pthread_mutex_lock(l);} while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(f);pthread_mutex_unlock(l);} while(0)
#define mutex_lock(l) pthread_mutex_lock(l)
#define mutex_unlock(l) pthread_mutex_unlock(l)
#define AN7552_NATIVE_CONTROL 0
#define BOOT_CONFIG 0
#define BOOT_TRIGGER 0
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define memset_io(...) ((void)0)
#define writel(...) ((void)0)
#define readl_poll_timeout(...) 0
#define usleep_range(...) ((void)0)
#define panic(...) do {assert(!"unexpected panic");__builtin_trap();} while(0)
struct an7552_npu {
    pthread_mutex_t setup_lock,attach_lock;
    int setup_wait;
    unsigned int setup_users;
    bool shutdown,shutdown_complete;
    bool running,faulted,fault_quiesced,failed,stopping,pn_failed,defer_queues,tdma_enabled;
    int queue_error,recovery_count;
    void *regs,*ram,*dev;
};
struct platform_device {struct an7552_npu *ctx;};
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static int panic_timeout,boots,starts,proofs,stop_entered,boot_entered,release_boot,release_stop;
static int block_boot,block_stop,destructor_done,shutdown_done;
static struct an7552_npu *platform_get_drvdata(struct platform_device *p){return p->ctx;}
static void wake_up_all(int *unused) {
    pthread_mutex_lock(&event_lock);pthread_cond_broadcast(&event);pthread_mutex_unlock(&event_lock);
}
#define wait_event(q,condition) do { \
    pthread_mutex_lock(&event_lock); \
    while(!(condition))pthread_cond_wait(&event,&event_lock); \
    pthread_mutex_unlock(&event_lock); \
} while(0)
static void await(int *flag) {
    for(unsigned i=0;i<10000000;i++) {if(READ_ONCE(*flag))return;sched_yield();}
    assert(!"actor never reached boundary");
}
static void await_closed(struct an7552_npu *c) {
    for(unsigned i=0;i<10000000;i++) {if(READ_ONCE(c->shutdown))return;sched_yield();}
    assert(!"gate never closed");
}
static void release(int *flag) {WRITE_ONCE(*flag,1);wake_up_all(0);}
static bool an7552_npu_healthy(struct an7552_npu *c){return !c->faulted;}
static int fault_stop(struct an7552_npu *c) {
    WRITE_ONCE(stop_entered,1);
    if(block_stop)wait_event(c->setup_wait,READ_ONCE(release_stop));
    c->fault_quiesced=true;c->tdma_enabled=false;return 0;
}
static void native_rro_stop(struct an7552_npu *c){}
static void native_txfree_stop(struct an7552_npu *c){}
static void tdma_stop(struct an7552_npu *c){c->tdma_enabled=false;}
static int halt_cores(struct an7552_npu *c){assert(!c->tdma_enabled);proofs++;return 0;}
static int boot_firmware(struct an7552_npu *c) {
    boots++;WRITE_ONCE(boot_entered,1);
    if(block_boot)wait_event(c->setup_wait,READ_ONCE(release_boot));
    c->running=true;c->faulted=false;c->fault_quiesced=false;c->stopping=false;return 0;
}
static int tdma_start(struct an7552_npu *c){starts++;c->tdma_enabled=true;return 0;}
'''

CASES = r'''
static void *shutdown_actor(void *arg) {
    an7552_npu_shutdown(arg);WRITE_ONCE(shutdown_done,1);return 0;
}
static void *prepare_actor(void *arg) {assert(!an7552_npu_prepare(arg,true));return 0;}
static void *stop_actor(void *arg) {an7552_npu_stop(arg);return 0;}
static void *destructor_actor(void *arg) {
    an7552_npu_require_quiescent(arg);WRITE_ONCE(destructor_done,1);return 0;
}
static void init(struct an7552_npu *c) {
    memset(c,0,sizeof(*c));pthread_mutex_init(&c->setup_lock,0);pthread_mutex_init(&c->attach_lock,0);
    c->running=true;c->faulted=true;c->fault_quiesced=true;
    boots=starts=proofs=stop_entered=boot_entered=release_boot=release_stop=0;
    block_boot=block_stop=destructor_done=shutdown_done=0;
}
int main(void) {
    struct an7552_npu c;pthread_t worker,shutdown,destructor;
    /* Original reproducer: a recoverable fault must not restart after shutdown. */
    init(&c);an7552_npu_shutdown(&c);
    assert(c.shutdown_complete&&!c.running&&proofs==1);
    assert(an7552_npu_prepare(&c,true)==-ESHUTDOWN);
    assert(an7552_npu_start(&c)==-ESHUTDOWN);
    assert(!an7552_npu_recovery_ready(&c)&&!boots&&!starts&&!c.setup_users);
    an7552_npu_require_quiescent(&c);assert(proofs==1);

    for(int stopped=0;stopped<2;stopped++) {
        /* Shutdown must wait for both admitted recovery and admitted stop. */
        init(&c);block_boot=!stopped;block_stop=stopped;
        // An in-flight boot may temporarily have running false. That cannot
        // release a concurrent destructor before shutdown_complete is set.
        if(!stopped)c.running=false;
        pthread_create(&worker,0,stopped?stop_actor:prepare_actor,&c);
        await(stopped?&stop_entered:&boot_entered);
        pthread_create(&shutdown,0,shutdown_actor,&c);await_closed(&c);
        pthread_create(&destructor,0,destructor_actor,&c);
        for(int i=0;i<10000;i++)sched_yield();
        assert(!READ_ONCE(shutdown_done)&&!READ_ONCE(destructor_done)&&!proofs);
        assert(!an7552_npu_recovery_ready(&c));
        assert(an7552_npu_start(&c)==-ESHUTDOWN);
        assert(an7552_npu_prepare(&c,true)==-ESHUTDOWN);
        release(stopped?&release_stop:&release_boot);
        pthread_join(worker,0);pthread_join(shutdown,0);pthread_join(destructor,0);
        assert(shutdown_done&&destructor_done&&c.shutdown_complete&&!c.running&&!c.setup_users);
        assert(proofs==1&&boots==!stopped&&starts==!stopped&&!c.tdma_enabled);
        pthread_mutex_destroy(&c.setup_lock);pthread_mutex_destroy(&c.attach_lock);
    }
    /* Admission itself is safe for an atomic caller: no sleeping mutex or wait
     * is added inside enter/exit; only the shutdown/destructor actors wait. */
    init(&c);assert(an7552_npu_setup_enter(&c));assert(c.setup_users==1);
    an7552_npu_setup_exit(&c);assert(!c.setup_users);
}
'''


class ShutdownTest(unittest.TestCase):
    def test_terminal_shutdown_drains_recovery_and_stop(self):
        with tempfile.TemporaryDirectory(prefix='an7552-shutdown-') as tmp:
            source = final_source(tmp)
            signatures = [
                'static bool an7552_npu_setup_enter(',
                'static void an7552_npu_setup_exit(',
                'static void an7552_npu_close_setup(',
                'static bool __an7552_npu_recovery_ready(',
                'bool an7552_npu_recovery_ready(',
                'static void __an7552_npu_stop(',
                'void an7552_npu_stop(',
                'static void __an7552_npu_require_quiescent(',
                'void an7552_npu_require_quiescent(',
                'static int __an7552_npu_prepare(',
                'int an7552_npu_prepare(',
                'static int __an7552_npu_start(',
                'int an7552_npu_start(',
                'void an7552_npu_shutdown(',
            ]
            code = HARNESS + '\n'.join(function(source,s) for s in signatures) + CASES
            cfile, binary = Path(tmp)/'test.c', Path(tmp)/'test'
            cfile.write_text(code)
            subprocess.run(shlex.split(os.getenv('HOSTCC','cc')) +
                           ['-std=gnu11','-O2','-Wall','-pthread',str(cfile),'-o',str(binary)],
                           check=True,capture_output=True)
            subprocess.run([str(binary)],check=True,capture_output=True,timeout=20)

    def test_every_setup_wrapper_releases_admission_on_error(self):
        with tempfile.TemporaryDirectory(prefix='an7552-shutdown-wrappers-') as tmp:
            source = final_source(tmp)
            code = HARNESS + '\n'.join(function(source, signature) for signature in (
                'static bool an7552_npu_setup_enter(',
                'static void an7552_npu_setup_exit(',
                'static void an7552_npu_close_setup('))
            code += '\nstatic int calls;\n'
            for ret, name in (('int','txfree_setup'), ('int','rro_setup'),
                              ('void','tx_setup'), ('int','queues_start')):
                signature = f'static {ret} __an7552_npu_{name}('
                body = function(source, signature)
                stub = '{ calls++;' + ('return -EIO;' if ret == 'int' else '') + '}\n'
                code += body[:body.index('{')] + stub
                code += function(source, f'{ret} an7552_npu_{name}(')
            code += r'''
int main(void) {
    struct an7552_npu c={};
    pthread_mutex_init(&c.setup_lock,0);
    assert(an7552_npu_txfree_setup(&c,0,0,0,0,0,0)==-EIO);
    assert(an7552_npu_rro_setup(&c,0,0,0,0,0,0,0,0,0,0,0)==-EIO);
    assert(an7552_npu_queues_start(&c)==-EIO);
    an7552_npu_tx_setup(&c,0,0,0);
    assert(calls==4&&!c.setup_users);
    an7552_npu_close_setup(&c);
    assert(an7552_npu_txfree_setup(&c,0,0,0,0,0,0)==-ESHUTDOWN);
    assert(an7552_npu_rro_setup(&c,0,0,0,0,0,0,0,0,0,0,0)==-ESHUTDOWN);
    assert(an7552_npu_queues_start(&c)==-ESHUTDOWN);
    an7552_npu_tx_setup(&c,0,0,0);
    assert(c.queue_error==-ESHUTDOWN&&calls==4&&!c.setup_users);
}
'''
            cfile, binary = Path(tmp)/'test.c', Path(tmp)/'test'
            cfile.write_text(code)
            subprocess.run(shlex.split(os.getenv('HOSTCC','cc')) +
                           ['-std=gnu11','-O2','-Wall','-pthread',str(cfile),'-o',str(binary)],
                           check=True,capture_output=True)
            subprocess.run([str(binary)],check=True,capture_output=True,timeout=20)


if __name__ == '__main__':
    unittest.main()
