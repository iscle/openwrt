#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual platform patch functions with DMA and NAND fault injection."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def postimage(path):
    return '\n'.join(line[1:] for line in (ROOT / path).read_text().splitlines()
                     if line[:1] in ('+', ' ') and not line.startswith('+++'))


def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_c(code):
    with tempfile.TemporaryDirectory(prefix='be5000-platform-') as tmp:
        source, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
        source.write_text(code)
        subprocess.run(shlex.split(os.getenv('HOSTCC', 'cc')) +
                       ['-std=gnu11', '-O2', '-Wall', str(source), '-o', str(binary)],
                       check=True, capture_output=True)
        subprocess.run([str(binary)], check=True, capture_output=True)


BMT = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;
typedef uint8_t u8;
#define AIROHA_MAX_BMT_ENTRIES 250
#define le16_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
struct airoha_bmt_entry {u16 bad_index,mapped_index;};
struct table {struct {u8 mapped_count;} header; struct airoha_bmt_entry entries[250];} table;
struct {struct table *bmt;} ctx={&table};
static int copy_error, oob_error, save_error, marked, persisted;
static int find_available_pool_block(bool end) {return 950;}
static int bbt_nand_copy(int d,int s,int n) {return copy_error;}
static int airoha_copy_block(int d,int s,int n) {return copy_error;}
static int bmt_find_by_mapped(int b) {
    for (int i=0;i<table.header.mapped_count;i++)
        if(table.entries[i].mapped_index==b)return i;
    return -1;
}
static int airoha_write_oob_idx(int b,int idx) {return oob_error;}
static int airoha_markbad(int b) {marked++;return 0;}
static int airoha_save_bmt(void) {persisted++;return save_error;}
'''
BMT_MAIN = r'''
int main(void) {
    for(int failure=0;failure<3;failure++) {
        memset(&table,0,sizeof(table));marked=persisted=0;
        copy_error=failure==0?-5:0;oob_error=failure==1?-5:0;save_error=failure==2?-5:0;
        assert(!airoha_remap_block(10,10,131072));
        assert(!marked && !table.header.mapped_count && !table.entries[0].mapped_index);
        table.header.mapped_count=1;table.entries[0]=(struct airoha_bmt_entry){10,940};
        assert(!airoha_remap_block(10,940,131072));
        assert(!marked && table.header.mapped_count==1 && table.entries[0].mapped_index==940);
    }
    copy_error=oob_error=save_error=0;marked=persisted=0;
    assert(airoha_remap_block(10,940,131072));
    assert(table.header.mapped_count==1 && table.entries[0].bad_index==10);
    assert(table.entries[0].mapped_index==950 && marked==1 && persisted==1);
}
'''
TX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uintptr_t dma_addr_t;
typedef int netdev_tx_t;
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
#define CHECKSUM_PARTIAL 1
#define DMA_TO_DEVICE 1
#define WARN_ON_ONCE(x) (x)
#define unlikely(x) (x)
#define FIELD_PREP(mask,x) ((u32)(x))
#define cpu_to_le32(x) (x)
#define WRITE_ONCE(x,y) ((x)=(y))
#define spin_lock_bh(x) ((void)0)
#define spin_unlock_bh(x) ((void)0)
#define dma_wmb() ((void)0)
#define QDMA_ETH_TXMSG_FPORT_MASK 1
#define QDMA_ETH_TXMSG_METER_MASK 2
#define QDMA_DESC_LEN_MASK 3
#define QDMA_DESC_NEXT_ID_MASK 4
#define QDMA_ETH_TXMSG_SP_TAG_MASK 5
#define TX_RING_CPU_IDX_MASK 6
#define REG_TX_CPU_IDX(x) (x)
struct list_head {struct list_head *next,*prev;};
#define list_first_entry(h,t,m) ((t *)((char *)(h)->next - offsetof(t,m)))
static void list_del_init(struct list_head *n) {n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
static void add_tail(struct list_head *h,struct list_head *n) {n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
struct netdev_queue {int stopped,sent;};
struct net_device {struct {void *parent;} dev;struct {int tx_packets,tx_bytes,tx_dropped;} stats;struct netdev_queue tx;};
struct sk_buff {void *data;unsigned len;int ip_summed;struct net_device *dev;};
struct airoha_queue_entry {struct list_head list;struct sk_buff *skb;dma_addr_t dma_addr;unsigned dma_len;};
struct airoha_qdma_desc {u32 ctrl,addr,data,msg0,msg1,msg2,msg3;};
struct airoha_queue {int lock,queued,ndesc,free_thr;bool txq_stopped;struct list_head tx_list;struct airoha_queue_entry *entry;struct airoha_qdma_desc *desc;};
struct airoha_qdma {struct airoha_queue q_tx[1];};
struct port {int id;};
struct airoha_gdm_dev {struct port *port;} gdm;
static int frees,maps,notify, map_error;
static void *netdev_priv(struct net_device *dev) {return &gdm;}
static struct netdev_queue *skb_get_tx_queue(struct net_device *d,struct sk_buff *s){return &d->tx;}
static int skb_linearize(struct sk_buff *s){return 0;}
static int skb_checksum_help(struct sk_buff *s){return 0;}
static int airoha_get_dsa_tag(struct sk_buff *s,struct net_device *d){s->len-=4;return 0;}
static dma_addr_t dma_map_single(void *d,void *p,unsigned n,int dir){maps++;return map_error?0:0x80000000+maps*2048;}
static int dma_mapping_error(void *d,dma_addr_t a){return !a;}
static void dev_kfree_skb_any(struct sk_buff *s){frees++;}
static void netif_tx_stop_queue(struct netdev_queue *q){q->stopped=1;}
static void netdev_tx_sent_queue(struct netdev_queue *q,unsigned n){q->sent++;}
static void skb_tx_timestamp(struct sk_buff *s){}
static void airoha_qdma_rmw(struct airoha_qdma *q,int r,u32 m,u32 v){notify++;}
'''
TX_MAIN = r'''
int main(void) {
    struct airoha_qdma dma={};struct airoha_queue *q=&dma.q_tx[0];
    struct airoha_queue_entry e[4]={};struct airoha_qdma_desc d[4]={};
    struct net_device dev={};struct port port={.id=2};struct sk_buff skb[5]={};
    gdm.port=&port;q->ndesc=4;q->free_thr=2;q->entry=e;q->desc=d;
    q->tx_list.next=q->tx_list.prev=&q->tx_list;
    for(int i=0;i<4;i++)add_tail(&q->tx_list,&e[i].list);
    for(int i=0;i<5;i++){skb[i].len=100;skb[i].dev=&dev;}
    for(int i=0;i<3;i++)assert(airoha_an7563_dev_xmit(&skb[i],&dev,&dma)==NETDEV_TX_OK);
    assert(!frees && maps==3 && q->queued==3 && notify==3);
    struct airoha_qdma_desc saved[4];memcpy(saved,d,sizeof(d));
    /* Arbitrarily delayed completion must preserve every outstanding DMA. */
    for(int i=0;i<10000;i++)assert(airoha_an7563_dev_xmit(&skb[3],&dev,&dma)==NETDEV_TX_BUSY);
    assert(!frees && maps==3 && !memcmp(saved,d,sizeof(d)) && q->queued==3 && skb[3].len==100);
    /* A completion releases its entry; only then can another submit proceed. */
    e[0].dma_addr=0;e[0].skb=0;add_tail(&q->tx_list,&e[0].list);q->queued--;
    assert(airoha_an7563_dev_xmit(&skb[3],&dev,&dma)==NETDEV_TX_OK);
    assert(e[1].skb==&skb[1] && e[2].skb==&skb[2] && e[3].skb==&skb[3]);
    e[1].dma_addr=0;e[1].skb=0;add_tail(&q->tx_list,&e[1].list);q->queued--;
    map_error=1;assert(airoha_an7563_dev_xmit(&skb[4],&dev,&dma)==NETDEV_TX_OK);
    assert(frees==1 && q->queued==2 && notify==4);
}
'''


class PlatformTests(unittest.TestCase):
    def test_kernel_bmt_remap_failures(self):
        source = postimage('target/linux/airoha/patches-6.18/900-airoha-bmt-support.patch')
        run_c(BMT + function(source, 'static bool airoha_remap_block(') + BMT_MAIN)

    def test_uboot_bmt_remap_failures(self):
        source = postimage('package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch')
        run_c(BMT + function(source, 'static bool airoha_remap_block(') + BMT_MAIN)

    def test_metadata_copy_on_write_powercuts(self):
        for path in ('target/linux/airoha/patches-6.18/900-airoha-bmt-support.patch',
                     'package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch'):
            source = postimage(path)
            code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define AIROHA_BBT_SIG_LEN 4
struct airoha_bmt_table {char signature[4]; unsigned value; u8 pad[24];};
struct {int system_blks,total_blks,bmt_block,bbt_block;u8 data_buf[2048];} ctx;
static struct {u8 data_buf[2048];} bmtd;
static struct airoha_bmt_table disk[8], before, next;
static int writes, erases, write_error, erase_error, corrupt_write, fatal;
static int cut, event, direction, erase_fail_after_write, bad_oob;
static jmp_buf escape;
static void point(void) {if (++event == cut) longjmp(escape, 1);}
static int airoha_bad_markers(int b) {return writes && bad_oob==1;}
static int airoha_read_oob_idx(int b,u16 *idx) {*idx=(writes && bad_oob==2)?7:0xffff;return 0;}
static int bmt_find_by_mapped(int b) {return -1;}
static int airoha_read_data(int b,void *out,size_t len){memcpy(out,&disk[b],len);return 0;}
static int bbt_nand_erase(int b) {
    erases++;
    if (erase_error==b || (erase_fail_after_write && writes)) return -EIO;
    memset(&disk[b],255,sizeof(disk[b]));point();return 0;
}
static int airoha_erase(int b) {return bbt_nand_erase(b);}
static int find_available_pool_block(bool end) {
    for(int n=0;n<8;n++) {int b=end?7-n:n;
        if(b==ctx.bmt_block || b==ctx.bbt_block)continue;
        if(!bbt_nand_erase(b))return b;
    }
    return -1;
}
static int airoha_write_data(int b,const void *data,size_t len) {
    writes++;memcpy(&disk[b],data,len);
    if(corrupt_write)disk[b].value=0;
    point();return write_error?-EIO:0;
}
static void airoha_metadata_ambiguous(void){fatal++;longjmp(escape,2);}
static int boot(void) {
    for(int n=0;n<8;n++){int b=direction?7-n:n;
        if(!memcmp(disk[b].signature,"BMT",3))return b;
    }
    return -1;
}
static void reset(int end) {
    memset(disk,255,sizeof(disk));memset(&before,0,sizeof(before));
    memcpy(before.signature,"BMT",4);before.value=123;
    next=before;next.value=456;
    direction=end;ctx.system_blks=0;ctx.total_blks=8;
    ctx.bbt_block=-1;ctx.bmt_block=end?5:2;disk[ctx.bmt_block]=before;
    writes=erases=write_error=corrupt_write=fatal=event=cut=0;erase_error=-1;
    erase_fail_after_write=bad_oob=0;
}
"""
            code += function(source, 'static int airoha_erase_metadata(')
            code += function(source, 'static int airoha_clear_stale_metadata(')
            code += function(source, 'static int airoha_commit_table(')
            code += r"""
int main(void) {
    for(int end=0;end<2;end++) {
        reset(end);int old=ctx.bmt_block;
        write_error=1;assert(airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end)==-EIO);
        assert(ctx.bmt_block==old && disk[boot()].value==123 && !fatal);
        reset(end);corrupt_write=1;
        assert(airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end)==-EIO);
        assert(disk[boot()].value==123 && !fatal);
        for(int marker=1;marker<=2;marker++) {
            reset(end);bad_oob=marker;
            assert(airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end)==-EIO);
            assert(disk[boot()].value==123 && !fatal);
        }
        reset(end);old=ctx.bmt_block;erase_error=old;
        assert(setjmp(escape) || (airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end),0));
        assert(fatal && disk[old].value==123 && disk[boot()].value==456);
        /* Simulate a cut after every erase/program boundary; reboot must
         * select a complete old/new table. A subsequent commit must not
         * resurrect an older duplicate when retiring the selected copy. */
        for(int boundary=1;boundary<=3;boundary++) {
            reset(end);cut=boundary;
            if(!setjmp(escape))assert(!airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end));
            int selected=boot();assert(selected>=0);
            assert(disk[selected].value==123 || disk[selected].value==456);
            ctx.bmt_block=selected;cut=0;next.value=789;
            assert(!airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end));
            assert(disk[boot()].value==789);
        }
        reset(end);write_error=erase_fail_after_write=1;
        /* A failed write can contain a valid preferred table. Cleanup
         * failure must never return into the caller's rollback/retry. */
        assert(setjmp(escape) || (airoha_commit_table(&next,sizeof(next),3,&ctx.bmt_block,end),0));
        assert(fatal && disk[boot()].value==456);
    }
}
"""
            run_c(code)

    def test_real_table_loaders_after_powercuts(self):
        for path in ('target/linux/airoha/patches-6.18/900-airoha-bmt-support.patch',
                     'package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch'):
            source = postimage(path)
            code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define pr_info(...) ((void)0)
#define printf(...) ((void)0)
#define AIROHA_MAX_BMT_ENTRIES 250
#define AIROHA_MAX_BBT_ENTRIES 250
#define AIROHA_BMT_SIG "BMT"
#define AIROHA_BBT_SIG "RAWB"
#define AIROHA_BMT_SIG_LEN 3
#define AIROHA_BBT_SIG_LEN 4
#define AIROHA_BMT_VERSION 1
#define AIROHA_BBT_VERSION 1
"""
            code += source[source.index('struct airoha_bmt_entry {'):
                           source.index('struct airoha_bmt_ctx {')]
            code += r"""
static struct airoha_bmt_table bmt;
static struct airoha_bbt_table bbt;
static struct {
    struct airoha_bmt_table *bmt;
    struct airoha_bbt_table *bbt;
    int system_blks,total_blks,pool_blks,bmt_block,bbt_block;
    u8 data_buf[2048];
} ctx={&bmt,&bbt,8,16,8,-1,-1};
static struct {u8 data_buf[2048];} bmtd;
static u8 disk[16][2048];
static int cut,event;
static jmp_buf escape;
static void point(void){if(++event==cut)longjmp(escape,1);}
static int airoha_bad_markers(int b){return 0;}
static bool airoha_isbad_any(int b){return false;}
static int airoha_read_oob_idx(int b,u16 *idx){*idx=0xffff;return 0;}
static int airoha_read_data(int b,void *out,size_t n){memcpy(out,disk[b],n);return 0;}
static int airoha_write_data(int b,const void *data,size_t n){memcpy(disk[b],data,n);point();return 0;}
static int bbt_nand_erase(int b){memset(disk[b],255,2048);point();return 0;}
static int airoha_erase(int b){return bbt_nand_erase(b);}
static void airoha_metadata_ambiguous(void){assert(0);}
"""
            for signature in ('static u8 bmt_calc_checksum(', 'static u32 bbt_calc_checksum(',
                              'static int bmt_find_by_mapped(',
                              'static int find_available_pool_block(',
                              'static bool bmt_entries_valid(', 'static bool bbt_entries_valid(',
                              'static int load_bmt(', 'static int load_bbt(',
                              'static int airoha_erase_metadata(',
                              'static int airoha_clear_stale_metadata(',
                              'static int airoha_commit_table(',
                              'static int airoha_save_bmt(', 'static int airoha_save_bbt('):
                if signature == 'static int find_available_pool_block(':
                    code += 'static int airoha_markbad(int b){return 0;}\n'
                code += function(source, signature) + '\n'
            code += r"""
static void boot(void) {
    ctx.bmt_block=ctx.bbt_block=-1;
    memset(&bmt,0,sizeof(bmt));memset(&bbt,0,sizeof(bbt));
    assert(!load_bmt() && !load_bbt());
    assert(bmt.header.checksum==bmt_calc_checksum(&bmt));
    assert(bbt.header.checksum==bbt_calc_checksum(&bbt));
}
static void seed(void) {
    cut=event=0;ctx.bmt_block=ctx.bbt_block=-1;
    memset(disk,255,sizeof(disk));memset(&bmt,0,sizeof(bmt));memset(&bbt,0,sizeof(bbt));
    bmt.header.mapped_count=1;bmt.entries[0].bad_index=2;bmt.entries[0].mapped_index=10;
    bbt.header.badblock_count=1;bbt.entries[0]=1;
    assert(!airoha_save_bmt() && !airoha_save_bbt());
    boot();
}
int main(void) {
    assert(sizeof(bmt)==1020 && sizeof(bbt)==512);
    for(int table=0;table<2;table++)for(int boundary=1;boundary<=3;boundary++) {
        seed();
        if(table)bbt.entries[0]=3;else bmt.entries[0].mapped_index=11;
        event=0;cut=boundary;
        if(!setjmp(escape))assert(!(table?airoha_save_bbt():airoha_save_bmt()));
        cut=0;boot();
        if(table) {
            assert(bmt.entries[0].mapped_index==10);
            assert(bbt.entries[0]==1 || bbt.entries[0]==3);
            bbt.entries[0]=4;assert(!airoha_save_bbt());boot();
            assert(bbt.entries[0]==4 && bmt.entries[0].mapped_index==10);
        } else {
            assert(bbt.entries[0]==1);
            assert(bmt.entries[0].mapped_index==10 || bmt.entries[0].mapped_index==11);
            bmt.entries[0].mapped_index=12;assert(!airoha_save_bmt());boot();
            assert(bmt.entries[0].mapped_index==12 && bbt.entries[0]==1);
        }
    }
}
"""
            run_c(code)

    def test_initial_tables_fail_closed(self):
        for path in ('target/linux/airoha/patches-6.18/900-airoha-bmt-support.patch',
                     'package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch'):
            run_c(r"""
#include <assert.h>
struct {int bmt_block,bbt_block;} ctx;
static int bmt_error,bbt_error,bmt_calls,bbt_calls;
static int airoha_save_bmt(void){bmt_calls++;return bmt_error;}
static int airoha_save_bbt(void){bbt_calls++;return bbt_error;}
""" + function(postimage(path), 'static int airoha_create_tables(') + r"""
int main(void){ctx.bmt_block=ctx.bbt_block=-1;bmt_error=-5;assert(airoha_create_tables()==-5 && !bbt_calls);bmt_error=0;bbt_error=-28;assert(airoha_create_tables()==-28);bbt_error=0;assert(!airoha_create_tables());ctx.bmt_block=950;ctx.bbt_block=940;bmt_calls=bbt_calls=0;assert(!airoha_create_tables() && !bmt_calls && !bbt_calls);}
""")

    def test_markbad_never_uses_factory_marker_on_pool(self):
        source = postimage('package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch')
        run_c(r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
typedef long long loff_t;
struct mtd_info {int unused;};
struct {int blk_shift,blk_size;} ctx={17,131072};
static int success, calls;
static int airoha_get_mapping_block(int b){return 950;}
static bool airoha_remap_block(int b,int m,int n){assert(b==10 && m==950);calls++;return success;}
""" + function(source, 'static int airoha_bmt_block_markbad(') + r"""
int main(void){struct mtd_info m;assert(airoha_bmt_block_markbad(&m,10LL<<17)==-EIO);success=1;assert(!airoha_bmt_block_markbad(&m,10LL<<17));assert(calls==2);}
""")

    def test_tx_backpressure_preserves_dma_ownership(self):
        source = postimage('target/linux/airoha/patches-6.18/960-net-airoha-retain-an7563-tx-dma-ownership.patch')
        run_c(TX + function(source, 'static netdev_tx_t airoha_an7563_dev_xmit(') + TX_MAIN)

    def test_tx_completion_255_empty_and_duplicate(self):
        source = postimage('target/linux/airoha/patches-6.18/960-net-airoha-retain-an7563-tx-dma-ownership.patch')
        harness = TX.replace('struct airoha_qdma {struct airoha_queue q_tx[1];};', r'''
struct napi_struct {int unused;};
struct airoha_eth {void *dev;};
struct airoha_tx_irq_queue {struct napi_struct napi;struct airoha_qdma *qdma;unsigned size;u32 *q;};
struct airoha_qdma {struct airoha_queue q_tx[1];struct airoha_tx_irq_queue q_tx_irq[1];struct airoha_eth *eth;int irq_banks[1];};
''')
        harness += r'''
#define U32_MAX UINT32_MAX
#define READ_ONCE(x) (x)
#define le32_to_cpu(x) (x)
#define dma_rmb() ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define FIELD_GET(m,x) (((x)&(m))/((m)&-(m)))
#define IRQ_HEAD_IDX_MASK 0x00000fffU
#define IRQ_ENTRY_LEN_MASK 0x0fff0000U
#define IRQ_RING_IDX_MASK 0x001f0000U
#define IRQ_DESC_IDX_MASK 0x0000ffffU
#define QDMA_DESC_DONE_MASK (1U<<31)
#define QDMA_DESC_DROP_MASK (1U<<30)
#define REG_IRQ_STATUS(x) 0
#define REG_IRQ_CLEAR_LEN(x) 0
#define IRQ_CLEAR_LEN_MASK 1
#define QDMA_INT_REG_IDX0 0
#define TX_DONE_INT_MASK(x) 1
#define list_add_tail(n,h) add_tail(h,n)
static int unmapped,completed;
static u32 irq_status;
static u32 airoha_qdma_rr(struct airoha_qdma *q,int r){return irq_status;}
static void dma_unmap_single(void *d,dma_addr_t a,unsigned n,int dir){unmapped++;}
static void netdev_tx_completed_queue(struct netdev_queue *q,int n,unsigned len){completed+=n;}
static void airoha_qdma_wake_netdev_txqs(struct airoha_queue *q){q->txq_stopped=false;}
static bool napi_complete(struct napi_struct *n){return true;}
static void airoha_qdma_irq_enable(int *b,int r,int m){}
'''
        run_c(harness + function(source, 'static int airoha_qdma_tx_napi_poll(') + r'''
int main(void){
    struct airoha_qdma dma={};struct airoha_eth eth={};
    struct airoha_queue_entry e[1024]={};struct airoha_qdma_desc d[1024]={};
    struct net_device dev={};struct sk_buff skb={.len=100,.dev=&dev};u32 irq[4]={255,U32_MAX};
    struct airoha_queue *q=&dma.q_tx[0];struct airoha_tx_irq_queue *iq=&dma.q_tx_irq[0];
    dma.eth=&eth;iq->qdma=&dma;iq->size=4;iq->q=irq;
    q->entry=e;q->desc=d;q->ndesc=1024;q->queued=2;q->free_thr=2;
    q->tx_list.next=q->tx_list.prev=&q->tx_list;
    e[255].skb=&skb;e[255].dma_addr=0x80000000;e[255].dma_len=100;d[255].ctrl=QDMA_DESC_DONE_MASK;
    irq_status=1U<<16;
    assert(airoha_qdma_tx_napi_poll(&iq->napi,8)==1);
    assert(unmapped==1 && frees==1 && completed==1 && q->queued==1 && irq[0]==U32_MAX);
    irq[0]=255;assert(airoha_qdma_tx_napi_poll(&iq->napi,8)==1);
    assert(unmapped==1 && frees==1 && completed==1 && q->queued==1);
    irq[0]=U32_MAX;int old_notify=notify;
    assert(!airoha_qdma_tx_napi_poll(&iq->napi,8) && notify==old_notify);
}
''')

    def test_uboot_table_corrected_ecc_and_short_read(self):
        source = postimage('package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch')
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#define MTD_OPS_PLACE_OOB 0
#define mtd_is_bitflip(r) ((r)==-EUCLEAN)
struct mtd_oob_ops {int mode,ooboffs,ooblen;void *oobbuf,*datbuf;size_t len,retlen;};
static int result,short_read;
static int read_oob(void *m,unsigned long a,struct mtd_oob_ops *o){o->retlen=o->len-short_read;return result;}
struct {int oob_offset;void *mtd;int (*orig_read_oob)(void*,unsigned long,struct mtd_oob_ops*);} ctx={.orig_read_oob=read_oob};
static unsigned long block_addr(int block){return block*131072UL;}
''' + function(source, 'static int airoha_read_data(') + r'''
int main(void){char data[100];result=4;assert(!airoha_read_data(1,data,100));result=-EUCLEAN;assert(!airoha_read_data(1,data,100));result=-EIO;assert(airoha_read_data(1,data,100)==-EIO);result=0;short_read=1;assert(airoha_read_data(1,data,100)==-EIO);}
''')


if __name__ == '__main__':
    unittest.main()
