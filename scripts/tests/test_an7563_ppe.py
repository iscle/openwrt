#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute production AN7563 PPE blocks with mocked packets and registers.

These tests cover the driver's policy and sequencing, not silicon behaviour.
The MAC checksum-error register meanings are independently confirmed against
stock fe_core's fe_api_set_drop_{tcp,udp,ip}_chksum_err_enable functions.
"""
import re
import unittest

from test_be5000_platform import function, postimage, run_c

PATCHES = 'target/linux/airoha/patches-6.18/'
ETH = postimage(PATCHES + '951-net-airoha-add-AN7563-basic-ethernet-support.patch')
PPE = postimage(PATCHES + '955-net-airoha-add-an7563-ppe.patch')
COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint16_t __be16;
#define BIT(n) (1U << (n))
'''


class AN7563PPE(unittest.TestCase):
    def test_checksum_validation_survives_both_mac_initializers(self):
        start = ETH.index('static void airoha_fe_maccr_init')
        branch = function(ETH[start:], 'if (eth->soc->version == 0x7563)')
        late = re.search(r'if \(eth->soc->version == 0x7563\)\n\s*airoha_fe_wr\(eth, REG_GDM_FWD_CFG\(AIROHA_GDM1_IDX\),.*?;', ETH, re.S).group()
        run_c(COMMON + r'''
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GDM_PAD_EN_MASK BIT(28)
#define GDM_TCP_CKSUM_MASK BIT(21)
#define GDM_UDP_CKSUM_MASK BIT(20)
#define GDM_IP4_CKSUM_MASK BIT(22)
#define GDM_DROP_CRC_ERR_MASK BIT(23)
#define AIROHA_GDM1_IDX 1
#define REG_GDM_FWD_CFG(p) (p)
struct soc {int version;};
struct airoha_eth {struct soc *soc; int ports[4]; u32 regs[5];};
static void airoha_fe_wr(struct airoha_eth *eth, int r, u32 v) {eth->regs[r]=v;}
static void initial(struct airoha_eth *eth) {int p;
''' + branch + '\n}\nstatic void late(struct airoha_eth *eth) {\n' + late + r'''
}
int main(void) {
    struct soc soc={0x7563}; struct airoha_eth eth={.soc=&soc};
    const u32 validation=BIT(20)|BIT(21)|BIT(22)|BIT(23);
    initial(&eth);
    for(int p=1;p<=4;p++) assert((eth.regs[p]&validation)==validation);
    late(&eth);
    for(int p=1;p<=4;p++) assert((eth.regs[p]&validation)==validation);
    /* Other SoCs must retain their existing setup path. */
    memset(eth.regs,0,sizeof(eth.regs));soc.version=0x7581;
    initial(&eth);late(&eth);
    for(int p=1;p<=4;p++) assert(!eth.regs[p]);
}
''')

    def test_routed_offloads_rejected_only_on_native_bridge_soc(self):
        # Execute the actual early-rejection block before any rule allocation.
        guard = re.search(r'if \(airoha_is_7563\(eth\) && addr_type\)\n\s*return -EOPNOTSUPP;', PPE).group()
        run_c(COMMON + r'''
struct airoha_eth {int version;};
static bool airoha_is_7563(struct airoha_eth *eth) {return eth->version==0x7563;}
static int accept_flow(struct airoha_eth *eth,u16 addr_type) {
''' + guard + r'''
return 0;
}
int main(void) {
    struct airoha_eth eth={0x7563};
    assert(!accept_flow(&eth,0)); /* MAC-only bridge rules stay eligible. */
    assert(accept_flow(&eth,AF_INET)==-EOPNOTSUPP);
    assert(accept_flow(&eth,AF_INET6)==-EOPNOTSUPP);
    eth.version=0x7581;
    assert(!accept_flow(&eth,0));assert(!accept_flow(&eth,AF_INET));assert(!accept_flow(&eth,AF_INET6));
}
''')

    def test_native_matcher_rejects_ambiguous_or_stale_keys(self):
        matcher = function(PPE, 'static bool airoha_ppe_native_bridge_match')
        defines = '\n'.join(line for line in PPE.splitlines() if line.startswith('#define AN7563_L2_KEY_'))
        run_c(COMMON + defines + r'''
#define ETH_HLEN 14
#define ETH_P_IP 0x0800
#define ETH_P_IPV6 0x86dd
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
struct ethhdr {unsigned char h_dest[6],h_source[6];u16 h_proto;};
struct vlan_hdr {u16 h_vlan_TCI,h_vlan_encapsulated_proto;};
struct sk_buff {struct ethhdr eh;struct vlan_hdr vh;bool tagged,truncated;u16 vlan_proto,tci;int port;};
struct airoha_ppe {int unused;};
struct airoha_foe_entry {struct {u32 key[5];} bridge_an7563;};
#define eth_hdr(skb) (&(skb)->eh)
#define skb_mac_offset(skb) 0
#define skb_vlan_tag_present(skb) ((skb)->tagged)
#define skb_vlan_tag_get(skb) ((skb)->tci)
static u32 get_unaligned_be32(const void *p) {u32 n;memcpy(&n,p,4);return ntohl(n);}
static u16 get_unaligned_be16(const void *p) {u16 n;memcpy(&n,p,2);return ntohs(n);}
static bool eth_type_vlan(u16 proto) {return proto==htons(ETH_P_8021Q)||proto==htons(ETH_P_8021AD);}
static int airoha_ppe_bridge_source_port(struct airoha_ppe *p,struct sk_buff *skb) {return skb->port;}
static void *skb_header_pointer(struct sk_buff *skb,int off,int size,void *buf) {
    assert(off==ETH_HLEN&&size==sizeof(skb->vh));return skb->truncated?0:&skb->vh;
}
''' + matcher + r'''
int main(void) {
    struct airoha_ppe ppe={0};struct sk_buff skb={.port=2};
    struct airoha_foe_entry hwe={0};u32 *key=hwe.bridge_an7563.key;
    memcpy(skb.eh.h_dest,"ABCDEF",6);memcpy(skb.eh.h_source,"abcdef",6);
    key[0]=get_unaligned_be32(skb.eh.h_dest);key[1]=(u32)get_unaligned_be16(skb.eh.h_dest+4)<<16;
    key[3]=get_unaligned_be32(skb.eh.h_source);key[4]=(u32)get_unaligned_be16(skb.eh.h_source+4)<<16;
    for(int ipv6=0;ipv6<2;ipv6++) {
        u16 proto=ipv6?ETH_P_IPV6:ETH_P_IP;
        u32 ipkey=ipv6?AN7563_L2_KEY_IPV6:AN7563_L2_KEY_IPV4;
        skb.eh.h_proto=htons(proto);key[1]&=0xffff0000;key[2]=ipkey|2;
        assert(airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
        /* Every MAC and qualifier word participates in identity. */
        for(int i=0;i<5;i++) {u32 bit=i==4?BIT(16):1;key[i]^=bit;
            assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));key[i]^=bit;}
        skb.port=-1;assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));skb.port=2;
        for(int stag=0;stag<2;stag++) {
            u16 tpid=stag?ETH_P_8021AD:ETH_P_8021Q;
            key[1]|=123;key[2]=ipkey|2|(stag?AN7563_L2_KEY_STAG:AN7563_L2_KEY_CTAG);
            skb.eh.h_proto=htons(tpid);skb.vh=(struct vlan_hdr){htons(123),htons(proto)};
            assert(airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
            skb.truncated=true;assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));skb.truncated=false;
            skb.vh.h_vlan_encapsulated_proto=htons(ETH_P_8021Q);
            assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
            skb.eh.h_proto=htons(proto);skb.tagged=true;skb.vlan_proto=htons(tpid);skb.tci=123;
            assert(airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
            skb.eh.h_proto=htons(ETH_P_8021Q);assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
            skb.tagged=false;key[1]&=0xffff0000;skb.eh.h_proto=htons(proto);
        }
        for(int domain=1;domain<=2048;domain++) {
            key[1]=(key[1]&0xffff0000)|domain;key[2]=ipkey|7|AN7563_L2_KEY_CTAG;
            assert(airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,domain));
            assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,domain==2048?1:domain+1));
        }
        assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,2049));
        skb.tagged=true;assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,2048));skb.tagged=false;
        skb.eh.h_proto=htons(ETH_P_8021Q);assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,2048));
    }
    skb.eh.h_proto=htons(0x0806);assert(!airoha_ppe_native_bridge_match(&ppe,&hwe,&skb,0));
}
''')


if __name__ == '__main__':
    unittest.main()
