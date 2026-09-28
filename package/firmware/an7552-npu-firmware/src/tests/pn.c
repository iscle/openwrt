/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../protocol.h"
static jmp_buf trap_state;
static unsigned int trap_after;
static void barrier(void)
{
	__sync_synchronize();
	if (trap_after && !--trap_after)
		longjmp(trap_state, 1);
}
#include "../pn.c.inc"
static uint32_t read32(uint32_t a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}
static uint32_t packet_word(volatile uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
#include "../rx-security.c.inc"
static void put32(uint8_t *p, uint32_t n)
{
	p[0] = n;
	p[1] = n >> 8;
	p[2] = n >> 16;
	p[3] = n >> 24;
}
int main(void)
{
	const uint32_t zero[34] = {};
	volatile uint32_t *q, *entry;
	assert(mmap((void *)0x44110000, 0xa0000, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)0x44110000);
	assert(mmap((void *)(uintptr_t)AN7552_NPU_BASE, 4096,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)AN7552_NPU_BASE);
	assert(mmap((void *)0x44300000, 0x100000, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)0x44300000);
	q = (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
	assert(pn_compare(2, 0, 1, 0) == 2);
	q[0] = 1;
	q[1] = 2;
	q[2] = 2;
	q[6] = 1;
	native_pn_service();
	assert(q[8] == 1 && q[7] == 1);
	assert(pn_compare(2, 0, 0, 0) == 0);
	assert(pn_compare(2, 0, 1, 0) == 1);
	assert(pn_compare(2, 0, 1, 0) == 0);
	assert(pn_compare(2, 0, 0xffffffff, 0) == 1);
	/* Interrupt a wrapping update at every publication boundary. Recovery
	 * may discard more packets, but must not accept any previously seen PN.
	 */
	for (volatile unsigned int boundary = 1; boundary <= 2; boundary++) {
		entry = pn_entry(2);
		pn_domain_lookup(entry[2], entry[3])[4] = UINT32_MAX;
		pn_domain_lookup(entry[2], entry[3])[5] = 0;
		trap_after = boundary;
		if (!setjmp(trap_state))
			(void)pn_compare(2, 0, 0, 1);
		trap_after = 0;
		assert(pn_compare(2, 0, UINT32_MAX, 0) == 0);
	}
	entry = pn_entry(2);
	pn_domain_lookup(entry[2], entry[3])[4] = UINT32_MAX;
	pn_domain_lookup(entry[2], entry[3])[5] = 0;
	assert(pn_compare(2, 0, 0, 1) == 1);
	assert(pn_compare(2, 0, 0xffffffff, 0) == 0);
	assert(pn_compare(2, 15, 1, 0) == 1 && pn_compare(2, 16, 1, 0) == 1);
	assert(pn_compare(2, 17, 1, 0) == 3 && pn_compare(2048, 0, 1, 0) == 3);
	assert(pn_compare(2, 0, 1, 65536) == 3);
	q[0] = 2;
	q[6] = 0;
	native_pn_service();
	assert(pn_compare(2, 0, 2, 0) == 2);
	q[0] = 3;
	q[6] = 1;
	native_pn_service();
	assert(pn_compare(2, 0, 1, 0) == 1);
	entry = pn_entry(2);
	assert(entry[1] == 3);
	q[0] = 4;
	q[1] = 1;
	q[3] = 0;
	q[4] = 2;
	q[5] = 0;
	native_pn_service();
	assert(q[7] == 1 && q[8] == 4);
	native_pn_service();
	assert(q[7] == 1);
	q[0] = 5;
	native_pn_service();
	assert(q[7] == 0 && q[8] == 5);
	q[0] = 6;
	q[9] = 1;
	native_pn_service();
	assert(q[8] == 5);
	q[9] = 0;
	/* A recovery snapshot revokes RX until reinstall, and retains all TIDs. */
	volatile uint32_t *saved = (void *)(uintptr_t)0x441a0000;
	for (unsigned int i = 1; i < 17; i++)
		assert(pn_compare(2, i, i + 23, 42 + i) == 1);
	q[0] = 7;
	q[1] = 3;
	q[6] = 0;
	native_pn_service();
	assert(q[8] == 7 && q[7] == 1 && !entry[0]);
	assert(saved[0] == 2 && saved[1] == 0);
	for (unsigned int i = 1; i < 17; i++)
		assert(saved[i * 2] == i + 23 && saved[i * 2 + 1] == 42 + i);
	assert(pn_compare(2, 0, 3, 0) == 2);
	q[0] = 8;
	q[1] = 2;
	q[6] = 1;
	native_pn_service();
	assert(q[7] == 1 && pn_compare(2, 0, 2, 0) == 0);
	for (unsigned int i = 1; i < 17; i++) {
		assert(pn_compare(2, i, i + 23, 42 + i) == 0);
		assert(pn_compare(2, i, i + 24, 42 + i) == 1);
	}
	assert(pn_compare(2, 0, 3, 0) == 1);
	q[0] = 9;
	q[1] = 3;
	q[6] = 1;
	native_pn_service();
	assert(q[7] == 2 && entry[0] == 1); /* Wrong key must not revoke it. */
	q[0] = 10;
	q[6] = 0;
	native_pn_service();
	assert(q[7] == 1 && saved[0] == 3 && !entry[0]);
	q[0] = 11;
	native_pn_service();
	assert(q[7] == 2);
	uint8_t packet[128] = { 0 };
	uint32_t cookie;
	put32(packet, 0x10000080);
	put32(packet + 4, 0x80090003);
	put32(packet + 8, 0x00040080);
	put32(packet + 32, 0x4188);
	put32(packet + 40, 0x0030);
	put32(packet + 48, 1);
	assert(pn_install(3, 1, 0, zero));
	pn_entry(3)[1] = 9;
	assert(native_rx_security(packet, 128, &cookie) == 0x31 && cookie == 9);
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	put32(packet + 48, 2);
	packet[16] = 3;
	assert(native_rx_security(packet, 128, &cookie) == 0x31);
	packet[16] = 2;
	assert(native_rx_security(packet, 128, &cookie) == 0x31);
	packet[16] = 1;
	assert(native_rx_security(packet, 128, &cookie) == 0x31);
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[16] = 0;
	put32(packet + 48, 3);
	packet[6] |= 0x20;
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[6] &= ~0x20;
	assert(native_rx_security(packet, 128, &cookie) == 0x31);
	packet[16] = 3;
	put32(packet + 48, 4);
	packet[64] = 0xaa;
	packet[65] = 0xaa;
	packet[66] = 3;
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[16] = 1;
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[64] = 2;
	packet[65] = 0;
	packet[66] = 0;
	packet[16] = 3;
	put32(packet + 48, 5);
	assert(native_rx_security(packet, 128, &cookie) == 0x31);
	/* Rejecting a replayed first subframe must invalidate the incomplete
	 * aggregate. A following subframe cannot inherit its old acceptance.
	 */
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[16] = 2;
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[16] = 1;
	assert(native_rx_security(packet, 128, &cookie) == 0x32);
	packet[16] = 0;
	for (unsigned int cipher = 0; cipher < 32; cipher++) {
		put32(packet + 8, (cipher << 16) | 0x80);
		put32(packet + 48, 10 + cipher);
		if (cipher == 4 || cipher == 10 || cipher == 11 || cipher == 12) {
			assert(native_rx_security(packet, 128, &cookie) == 0x31);
			assert(native_rx_security(packet, 128, &cookie) == 0x32);
		} else {
			assert(native_rx_security(packet, 128, &cookie) == 0);
		}
	}
	/* The QoS metadata field is undefined for non-QoS data. */
	put32(packet + 8, 0x000c0080);
	put32(packet + 32, 0x4108);
	put32(packet + 40, 0x00050030);
	put32(packet + 48, 1000);
	assert(native_rx_security(packet, sizeof(packet), &cookie) == 0x31);
	assert(pn_entry(3)[4] == 1000);
	assert(pn_entry(3)[4 + 5 * 2] == 0);
	/* A raw QoS frame without group 4 uses the actual 802.11 header.
	 * In particular, data must not advance the robust-management counter.
	 */
	uint8_t raw[128] = { 0 };
	put32(raw, 0x10000080);
	put32(raw + 4, 0x80010004);
	put32(raw + 8, 0x000c0000);
	put32(raw + 32, 100);
	put32(raw + 48, 0x4188);
	put32(raw + 68, 0x00700000);
	raw[72] = 5;
	assert(pn_install(4, 1, 0, zero));
	assert(native_rx_security(raw, sizeof(raw), &cookie) == 0x41);
	assert(pn_entry(4)[4 + 5 * 2] == 100);
	assert(pn_entry(4)[4 + 16 * 2] == 0);
	assert(native_rx_security(raw, sizeof(raw), &cookie) == 0x42);
	/* Truncated raw header must not change any replay state. */
	put32(raw + 32, 101);
	assert(native_rx_security(raw, 72, &cookie) == 0);
	assert(pn_entry(4)[4 + 5 * 2] == 100);
	/* A genuine protected management frame has its own replay domain. */
	put32(raw + 48, 0x40d0);
	assert(native_rx_security(raw, sizeof(raw), &cookie) == 0x41);
	assert(pn_entry(4)[4 + 16 * 2] == 101);
	/* Non-QoS data uses TID 0, regardless of the preceding QoS frame. */
	put32(raw + 48, 0x4108);
	assert(native_rx_security(raw, sizeof(raw), &cookie) == 0x41);
	assert(pn_entry(4)[4] == 101);
	/* Optional receive groups and padding precede the actual header. */
	uint8_t grouped[256] = { 0 };
	put32(grouped, 0x10000100);
	put32(grouped + 4, 0x801f0005);
	put32(grouped + 8, 0x000c2000);
	put32(grouped + 32, 0x40d0); /* Group 4 must not override raw QoS. */
	put32(grouped + 48, 200);
	put32(grouped + 194, 0x4388); /* Four-address QoS data. */
	grouped[224] = 7;
	assert(pn_install(5, 1, 0, zero));
	for (unsigned int size = 0; size < 226; size++)
		assert(native_rx_security(grouped, size, &cookie) == 0);
	assert(native_rx_security(grouped, sizeof(grouped), &cookie) == 0x51);
	assert(pn_entry(5)[4 + 7 * 2] == 200);
	assert(pn_entry(5)[4 + 16 * 2] == 0);
	puts("PASS: PN replay/duplicate/carry, TID isolation, key revocation/reset, bounds and command ordering");
}
