/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../tx-priority.h"

int main(void)
{
	uint8_t packet[96] = { 0 }, map[64];
	uint32_t priority;
	long page = sysconf(_SC_PAGESIZE);
	uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	assert(guard != MAP_FAILED);
	assert(!mprotect(guard + page, page, PROT_NONE));
	for (unsigned int i = 0; i < 64; i++)
		map[i] = i & 7; /* Deliberately differs from the default QoS map. */
	packet[12] = 8;
	packet[14] = 0x45;
	for (unsigned int dscp = 0; dscp < 64; dscp++) {
		for (unsigned int ecn = 0; ecn < 4; ecn++) {
			packet[15] = dscp * 4 + ecn;
			assert(packet_priority(packet, 34, map, &priority));
			assert(priority == map[dscp]);
		}
	}
	for (unsigned int len = 0; len < 34; len++) {
		memcpy(guard + page - len, packet, len);
		assert(!packet_priority(guard + page - len, len, map, &priority));
	}
	packet[14] = 0x44;
	assert(!packet_priority(packet, 34, map, &priority));
	packet[14] = 0x4f;
	assert(!packet_priority(packet, 34, map, &priority));
	packet[12] = 0x86;
	packet[13] = 0xdd;
	for (unsigned int dscp = 0; dscp < 64; dscp++) {
		for (unsigned int ecn = 0; ecn < 4; ecn++) {
			unsigned int tc = dscp * 4 + ecn;

			packet[14] = 0x60 | (tc >> 4);
			packet[15] = (tc << 4) | 15;
			assert(packet_priority(packet, 54, map, &priority));
			assert(priority == map[dscp]);
		}
	}
	for (unsigned int len = 0; len < 54; len++) {
		memcpy(guard + page - len, packet, len);
		assert(!packet_priority(guard + page - len, len, map, &priority));
	}
	packet[12] = 0x81;
	packet[13] = 0;
	packet[14] = 6 << 5;
	packet[16] = 8;
	packet[17] = 0;
	packet[18] = 0x45;
	packet[19] = 40 << 2;
	assert(packet_priority(packet, 38, map, &priority) && priority == 6);
	packet[14] = 0;
	assert(packet_priority(packet, 38, map, &priority) && priority == map[40]);
	map[40] = 8;
	assert(!packet_priority(packet, 38, map, &priority));
	packet[12] = 0x88;
	packet[13] = 0x47;
	packet[16] = 5 << 1;
	assert(packet_priority(packet, 18, map, &priority) && priority == 5);
	assert(!packet_priority(packet, 17, map, &priority));
	packet[12] = 0x89;
	packet[13] = 0x17;
	assert(packet_priority(packet, 14, map, &priority) && priority == 7);
	puts("PASS: per-packet IPv4/IPv6 DSCP, ECN, custom map, VLAN, MPLS and guarded truncation");
	return 0;
}
