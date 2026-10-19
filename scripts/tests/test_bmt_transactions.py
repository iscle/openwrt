#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real remap/allocator/table transaction as one NAND operation."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_be5000_platform import ROOT, function, postimage

PATCHES = (
    'target/linux/airoha/patches-6.18/900-airoha-bmt-support.patch',
    'package/boot/uboot-airoha/patches/600-mtd-spi-nand-add-airoha-bmt-support.patch',
)


def execute(code):
    with tempfile.TemporaryDirectory(prefix='bmt-transaction-') as tmp:
        source, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
        source.write_text(code)
        subprocess.run(shlex.split(os.getenv('HOSTCC', 'cc')) +
                       ['-std=gnu11', '-O2', '-Wall', str(source), '-o', str(binary)],
                       check=True, capture_output=True)
        return subprocess.run([str(binary)], capture_output=True)


def transaction_model(source):
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define printf(...) ((void)0)
#define AIROHA_MAX_BMT_ENTRIES 250
#define AIROHA_MAX_BBT_ENTRIES 250
#define AIROHA_BMT_SIG "BMT"
#define AIROHA_BBT_SIG "RAWB"
#define AIROHA_BMT_SIG_LEN 3
#define AIROHA_BBT_SIG_LEN 4
#define AIROHA_BMT_VERSION 1
#define AIROHA_BBT_VERSION 1
'''
    code += source[source.index('struct airoha_bmt_entry {'):
                   source.index('struct airoha_bmt_ctx {')]
    code += r'''
static struct airoha_bmt_table bmt;
static struct airoha_bbt_table bbt;
static struct {
    struct airoha_bmt_table *bmt;
    struct airoha_bbt_table *bbt;
    int system_blks,total_blks,pool_blks,bmt_block,bbt_block,reserved_block;
    u8 data_buf[2048];
} ctx={&bmt,&bbt,8,16,8,-1,-1,-1};
static struct {u8 data_buf[2048];} bmtd;
static u8 disk[16][2048], bad[16];
static u16 recovery_index[16];
static int cut,event,write_failure,copy_failure,oob_failure,source_erased;
static jmp_buf escape;
static void point(void){if(++event==cut)longjmp(escape,1);}
static int airoha_bad_markers(int b){return bad[b];}
static bool airoha_isbad_any(int b){return bad[b];}
static int airoha_markbad(int b){bad[b]=1;point();return 0;}
static int airoha_read_oob_idx(int b,u16 *idx){*idx=recovery_index[b];return 0;}
static int airoha_write_oob_idx(int b,u16 idx){recovery_index[b]=idx;point();return oob_failure?-EIO:0;}
static int airoha_read_data(int b,void *out,size_t n){memcpy(out,disk[b],n);return 0;}
static int airoha_write_data(int b,const void *data,size_t n){memcpy(disk[b],data,n);point();return write_failure?-EIO:0;}
static int bbt_nand_erase(int b){
    if(b==15)source_erased++;
    memset(disk[b],255,2048);recovery_index[b]=0xffff;point();return 0;
}
static int airoha_erase(int b){return bbt_nand_erase(b);}
static int bbt_nand_copy(int d,int s,int n){memcpy(disk[d],disk[s],2048);point();return copy_failure?-EIO:0;}
static int airoha_copy_block(int d,int s,int n){return bbt_nand_copy(d,s,n);}
static void airoha_metadata_ambiguous(void){assert(0);}
'''
    for signature in (
            'static u8 bmt_calc_checksum(', 'static u32 bbt_calc_checksum(',
            'static int bmt_find_by_mapped(', 'static int bmt_find_by_bad(',
            'static int find_available_pool_block(',
            'static bool bmt_entries_valid(', 'static bool bbt_entries_valid(',
            'static int load_bmt(', 'static int load_bbt(',
            'static int airoha_erase_metadata(',
            'static int airoha_clear_stale_metadata(',
            'static int airoha_commit_table(',
            'static int airoha_save_bmt(', 'static bool airoha_remap_block('):
        code += function(source, signature) + '\n'
    if 'static void airoha_unmap_block(' in source:
        code += function(source, 'static void airoha_unmap_block(') + '\n'
        code += '#define HAS_UNMAP 1\n'
    code += r'''
static void boot(void) {
    ctx.reserved_block=ctx.bmt_block=ctx.bbt_block=-1;
    memset(&bmt,0,sizeof(bmt));memset(&bbt,0,sizeof(bbt));
    assert(!load_bmt() && !load_bbt());
    int index=bmt_find_by_bad(2);assert(index>=0);
    int mapped=bmt.entries[index].mapped_index;
    /* A valid table must select intact data, not its own metadata or an erase. */
    assert(!bad[mapped] && disk[mapped][0]==0x5a);
}
static void seed(bool full) {
    cut=event=write_failure=copy_failure=oob_failure=source_erased=0;
    ctx.reserved_block=-1;ctx.bmt_block=14;ctx.bbt_block=8;
    memset(disk,255,sizeof(disk));memset(bad,0,sizeof(bad));
    memset(recovery_index,255,sizeof(recovery_index));
    memset(&bmt,0,sizeof(bmt));memset(&bbt,0,sizeof(bbt));
    memcpy(bmt.header.signature,"BMT",3);bmt.header.version=1;
    bmt.header.mapped_count=4;
    bmt.entries[0]=(struct airoha_bmt_entry){2,15};
    for(int i=1;i<4;i++)bmt.entries[i]=(struct airoha_bmt_entry){2+i,10+i};
    if(full)bmt.entries[bmt.header.mapped_count++]=(struct airoha_bmt_entry){6,10};
    bmt.header.checksum=bmt_calc_checksum(&bmt);memcpy(disk[14],&bmt,sizeof(bmt));
    memcpy(bbt.header.signature,"RAWB",4);bbt.header.version=1;
    bbt.header.checksum=bbt_calc_checksum(&bbt);memcpy(disk[8],&bbt,sizeof(bbt));
    memset(disk[15],0x5a,2048);recovery_index[15]=2;
    for(int i=1;i<bmt.header.mapped_count;i++)
        recovery_index[bmt.entries[i].mapped_index]=bmt.entries[i].bad_index;
}
int main(void) {
    /* Only one free pool block: a replacement fits, but metadata needs a
     * second spare. Refuse the transaction without erasing the old data. */
    seed(true);assert(!airoha_remap_block(2,15,131072));
    assert(!source_erased && !bad[15] && ctx.reserved_block==-1);boot();
    /* With two spares, metadata must never occupy the retiring source. */
    seed(false);assert(airoha_remap_block(2,15,131072));
    assert(!source_erased && ctx.bmt_block==10 && !bad[10] && bad[15]);
    assert(ctx.reserved_block==-1);boot();
    /* Successful NAND program reporting an error is cleaned up and rolled
     * back; neither the in-memory nor boot mapping may select erased data. */
    seed(false);write_failure=1;assert(!airoha_remap_block(2,15,131072));
    assert(!source_erased && ctx.reserved_block==-1 && bmt.entries[0].mapped_index==15);boot();
    seed(false);copy_failure=1;assert(!airoha_remap_block(2,15,131072));
    assert(!source_erased && ctx.reserved_block==-1);boot();
    seed(false);oob_failure=1;assert(!airoha_remap_block(2,15,131072));
    assert(!source_erased && ctx.reserved_block==-1);boot();
    /* Cut after every modeled erase/program/retirement boundary, then load
     * using the actual boot table scanners, not the in-memory snapshot. */
    seed(false);assert(airoha_remap_block(2,15,131072));int boundaries=event;
    for(int boundary=1;boundary<=boundaries;boundary++) {
        seed(false);cut=boundary;
        if(!setjmp(escape))assert(airoha_remap_block(2,15,131072));
        cut=0;boot();assert(!source_erased);
    }
#ifdef HAS_UNMAP
    /* The debug unmap operation also edits the map before persisting it.
     * Its failure must leave the old authoritative source readable. */
    seed(false);write_failure=1;airoha_unmap_block(2);
    assert(!source_erased && ctx.reserved_block==-1 && bmt.entries[0].mapped_index==15);boot();
    seed(false);airoha_unmap_block(2);
    assert(!source_erased && ctx.reserved_block==-1 && ctx.bmt_block!=15);
    assert(bmt_find_by_bad(2)<0 && disk[15][0]==0x5a);
#endif
    return 0;
}
'''
    return code


class BmtTransactionTests(unittest.TestCase):
    def test_linux_and_uboot_remap_transactions(self):
        for path in PATCHES:
            with self.subTest(path=path):
                result = execute(transaction_model(postimage(path)))
                self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_original_source_reuse_is_detected(self):
        # Negative control: removing the allocation reservation recreates the
        # reviewed bug and must fail the same data-preservation assertions.
        for path in PATCHES:
            source = postimage(path).replace(
                ' ||\n\t\t    block == ctx.reserved_block)', ')')
            with self.subTest(path=path):
                self.assertNotEqual(execute(transaction_model(source)).returncode, 0)

    def test_unmapped_second_device_preserves_existing_owner(self):
        source = postimage(PATCHES[0])
        start = source.index('int mtk_bmt_attach(')
        end = source.index('\tbmtd.remap_range =', start)
        # Execute the actual attach admission path up to allocation/setup.
        admission = source[start:end] + '\treturn 0;\n}\n'
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
struct mtk_bmt_ops {int id;};
static const struct mtk_bmt_ops mtk_bmt_v2_ops={1},mtk_bmt_nmbm_ops={2},
    mtk_bmt_bbt_ops={3},airoha_bmt_ops={4};
struct device_node {const char *property;};
struct mtd_info {struct device_node *np;};
static struct {struct mtd_info *mtd;const struct mtk_bmt_ops *ops;} bmtd;
static struct device_node *mtd_get_of_node(struct mtd_info *m){return m->np;}
static bool of_property_read_bool(struct device_node *n,const char *p){
    return n->property && !strcmp(n->property,p);
}
'''
        code += admission
        code += r'''
int main(void){
 struct device_node no_property={0},mapped={"mediatek,nmbm"};
 struct mtd_info owner={0},second={0};
 bmtd.mtd=&owner;bmtd.ops=&airoha_bmt_ops;
 assert(!mtk_bmt_attach(&second));
 assert(bmtd.mtd==&owner && bmtd.ops==&airoha_bmt_ops);
 second.np=&no_property;assert(!mtk_bmt_attach(&second));
 assert(bmtd.mtd==&owner && bmtd.ops==&airoha_bmt_ops);
 second.np=&mapped;assert(mtk_bmt_attach(&second)==-ENOSPC);
 assert(bmtd.mtd==&owner && bmtd.ops==&airoha_bmt_ops);
 bmtd.mtd=0;assert(!mtk_bmt_attach(&second));assert(bmtd.ops==&mtk_bmt_nmbm_ops);
}
'''
        result = execute(code)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        broken = code.replace('np = mtd_get_of_node(mtd);',
                              'if (bmtd.mtd) return -ENOSPC;\n\tnp = mtd_get_of_node(mtd);')
        self.assertNotEqual(execute(broken).returncode, 0)

    def test_spinand_probe_does_not_register_after_attachment_failure(self):
        source = postimage('target/linux/airoha/patches-6.18/901-snand-mtk-bmt-support.patch')
        start = source.index('\tret = mtk_bmt_attach(mtd);')
        end = source.index('\treturn ret;', start) + len('\treturn ret;')
        # This is the production probe tail, including registration and the
        # cleanup label. Model attachment restoring raw callbacks on error.
        tail = source[start:end]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
static int attach_error,registered,cleaned,detached;
static bool raw;
static int mtk_bmt_attach(void *m){raw=attach_error!=0;return attach_error;}
static int mtd_device_register(void *m,void *p,int n){registered++;return 0;}
static void mtk_bmt_detach(void *m){detached++;}
static void spinand_cleanup(void *s){cleaned++;}
static int probe(void){void *mtd=0,*spinand=0;int ret;
'''
        code += tail + '\n}\n'
        code += r'''
int main(void){
 for(int failure=0;failure<3;failure++){
  attach_error=failure==0?0:failure==1?-12:-5;registered=cleaned=detached=0;
  assert(probe()==attach_error);
  assert(registered==!failure && cleaned==!!failure && detached==!!failure);
  assert(!raw || !registered);
 }
}
'''
        result = execute(code)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        # Merely dropping the return-value check must expose the raw device.
        broken = code.replace('ret = mtk_bmt_attach(mtd);\n\tif (ret)\n\t\tgoto err_spinand_cleanup;',
                              'mtk_bmt_attach(mtd);')
        self.assertNotEqual(execute(broken).returncode, 0)


if __name__ == '__main__':
    unittest.main()
