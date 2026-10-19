/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"
static void barrier(void)
{
	__sync_synchronize();
}
static uint32_t read32(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}
static uint32_t packet_word(volatile uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
#include "../pn.c.inc"
#include "../rx-security.c.inc"
static void put32(uint8_t *p, uint32_t value)
{
	for (unsigned int i = 0; i < 4; i++)
		p[i] = value >> (i * 8);
}
static volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
static uint32_t command(uint32_t operation, uint32_t wcid, uint32_t arg,
			uint32_t enable)
{
	q[0]++;
	q[1] = operation;
	q[2] = wcid;
	q[3] = arg;
	q[6] = enable;
	native_pn_service();
	assert(q[8] == q[0]);
	return q[7];
}
static uint32_t install(uint32_t wcid, uint32_t handle)
{
	assert(command(2, wcid, handle, 1) == 1);
	assert(q[14] && pn_entry(wcid)[2] == q[14]);
	return q[14];
}
static uint32_t receive(uint32_t wcid, uint32_t band, uint32_t pn,
			uint32_t aggregate)
{
	uint8_t packet[128] = {};
	uint32_t cookie = 0, result;

	put32(packet, 0x10000080);
	put32(packet + 4, 0x80090000 | wcid | (band << 27));
	put32(packet + 8, 0x00040080);
	packet[16] = aggregate;
	put32(packet + 32, 0x4188);
	put32(packet + 40, 0x0030);
	put32(packet + 48, pn);
	result = native_rx_security(packet, sizeof(packet), &cookie);
	assert(cookie == pn_entry(wcid)[1]);
	return result & 15;
}
static void map(uint32_t start, uint32_t length)
{
	assert(mmap((void *)(uintptr_t)start, length, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		    0) == (void *)(uintptr_t)start);
}
int main(void)
{
	uint32_t shared, unrelated, saved_cookie;
	volatile uint32_t *staging = (void *)(uintptr_t)0x441a0000;

	map(0x44110000, 0xa0000);
	map(0x44300000, 0x100000);
	map(AN7552_NPU_BASE, 4096);
	shared = install(1, 0);
	assert(install(2, shared) == shared);
	unrelated = install(3, 0);
	assert(unrelated != shared);
	assert(receive(1, 0, 1, 0) == 1);
	assert(receive(2, 1, 1, 0) == 2);
	assert(receive(3, 1, 1, 0) == 1);
	assert(pn_compare(2, 0, 1, 0) == 0);
	/* One reorder WCID can have incomplete aggregates on both radios. */
	assert(receive(1, 0, 10, 3) == 1);
	assert(receive(1, 1, 11, 3) == 1);
	assert(receive(1, 0, 10, 2) == 1);
	assert(receive(1, 1, 11, 1) == 1);
	assert(receive(1, 0, 10, 1) == 1);
	assert(receive(1, 0, 10, 1) == 2);
	assert(receive(2, 0, 11, 0) == 2);
	assert(receive(1, 0, 12, 3) == 1);
	assert(receive(1, 0, 12, 3) == 2);
	assert(receive(1, 0, 12, 2) == 2);
	/* Both slow-path and native RX update the same counter. */
	assert(pn_compare(2, 0, 20, 0) == 1);
	assert(receive(1, 1, 20, 0) == 2);
	assert(receive(1, 1, 21, 0) == 1);
	assert(pn_compare(2, 0, 21, 0) == 0);
	assert(command(3, 1, 0, 0) == 1);
	assert(staging[0] == 21 && !pn_entry(1)[0]);
	assert(pn_compare(2, 0, 22, 0) == 1);
	saved_cookie = pn_entry(1)[1];
	assert(install(1, shared) == shared);
	assert(pn_entry(1)[1] != saved_cookie);
	assert(pn_compare(1, 0, 22, 0) == 0);
	assert(command(2, 1, 0, 0) == 1);
	assert(!pn_entry(1)[0] && !pn_entry(1)[2]);
	assert(pn_compare(2, 0, 22, 0) == 0);
	memset((void *)staging, 0, 34 * 4);
	assert(install(1, 0) != shared);
	assert(receive(1, 0, 1, 0) == 1);
	assert(receive(2, 0, 1, 0) == 2);
	/* Invalid key/domain handles revoke eligibility without joining peers. */
	assert(command(2, 1, shared, 1 | (1 << 8)) == 3);
	assert(!pn_entry(1)[0]);
	assert(command(2, 1, 0, 0) == 1);
	assert(command(2, 2, 0, 0) == 1);
	assert(!pn_domain_lookup(shared, 0));
	assert(command(2, 1, shared, 1) == 3);
	assert(!pn_entry(1)[0]);
	assert(command(2, AN7552_STATION_COUNT, 0, 0) == 3);
	pn_entry(4)[1] = UINT32_MAX;
	assert(command(2, 4, 0, 1) == 3 && !pn_entry(4)[0]);
	/* Staged links cannot accept packets before every snapshot is merged. */
	assert(command(2, 5, 0, 3) == 1);
	shared = q[14];
	assert(shared && !pn_entry(5)[0] && pn_entry(5)[60] == 1);
	staging[0] = 400;
	assert(command(2, 6, shared, 3) == 1);
	assert(!pn_entry(6)[0]);
	assert(pn_compare(5, 0, 399, 0) == 2);
	assert(pn_compare(6, 0, 401, 0) == 2);
	staging[0] = 5;
	staging[1] = 5;
	assert(command(4, 2, 0, 0) == 3);
	assert(!pn_entry(5)[0] && !pn_entry(6)[0]);
	staging[1] = 6;
	assert(command(4, 2, 0, 1) == 3);
	assert(!pn_entry(5)[0] && !pn_entry(6)[0]);
	assert(command(4, 2, 0, 0) == 1);
	assert(pn_entry(5)[0] == 1 && pn_entry(6)[0] == 1);
	assert(!pn_entry(5)[60] && !pn_entry(6)[60]);
	assert(pn_compare(5, 0, 400, 0) == 0);
	assert(pn_compare(6, 0, 401, 0) == 1);
	assert(pn_compare(5, 0, 401, 0) == 0);
	assert(command(4, 2, 0, 0) == 3);
	puts("PASS: actual RX path, cross-WCID replay, per-radio aggregates, host checks and recovery snapshots");
}
