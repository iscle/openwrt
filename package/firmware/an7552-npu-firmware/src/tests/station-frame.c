/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../protocol.h"
#include "../station.c.inc"

static void check(const uint8_t *frame, uint32_t length, int expected)
{
	/* A guard page catches even a single read past a truncated header. */
	size_t page = sysconf(_SC_PAGESIZE);
	uint8_t *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint8_t *packet;

	assert(mapping != MAP_FAILED && length < page);
	assert(!mprotect(mapping + page, page, PROT_NONE));
	packet = mapping + page - length;
	memcpy(packet, frame, length);
	assert(station_data_frame(packet, length) == expected);
	assert(!munmap(mapping, page * 2));
}

int main(void)
{
	/* Policy is host-owned once known, including across queued CPU traffic. */
	uint32_t *table =
		mmap((void *)(uintptr_t)AN7552_STATION_NPU, 4096,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	assert(table != MAP_FAILED);
	uint8_t data[14] = { 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 8, 0 };
	table[0] = AN7552_STATION_AUTHORIZED;
	table[1] = 0x301;
	table[2] = 0x05040302;
	table[3] = 0x0706;
	table[4] = 0x80;
	station_learn(1, 3, 0x88, data, sizeof(data));
	assert(table[4] == 0x80);
	table[4] = 0;
	station_learn(1, 3, 0x80, data, sizeof(data));
	assert(table[4] == 0x80);
	table[4] = 0;
	table[0] = AN7552_STATION_MLO;
	station_learn(1, 3, 0x88, data, sizeof(data));
	assert(table[4] == 0);
	table[0] = AN7552_STATION_AUTHORIZED;
	data[12] = 0x88;
	data[13] = 0x8e;
	station_learn(1, 3, 0x88, data, sizeof(data));
	assert(table[4] == 0);
	data[12] = 8;
	data[13] = 0;
	station_learn(1, 3, 0x88, data, sizeof(data));
	assert(table[4] == 0x88);
	assert(!munmap(table, 4096));
	uint8_t source[6] = { 2, 3, 4, 5, 6, 7 };
	uint32_t entry[8] = { AN7552_STATION_AUTHORIZED, 0x301, 0x05040302,
			      0x0706 };

	assert(station_rx_authorized(entry, 1, source));
	assert(!station_rx_authorized(entry, 0, source));
	entry[0] |= AN7552_STATION_MLO;
	assert(station_rx_authorized(entry, 0, source));
	assert(station_rx_authorized(entry, 1, source));
	assert(!station_rx_authorized(entry, 2, source));
	source[5] ^= 1;
	assert(!station_rx_authorized(entry, 0, source));
	assert(!station_rx_authorized(entry, 1, source));
	source[5] ^= 1;
	entry[0] = AN7552_STATION_MLO;
	assert(!station_rx_authorized(entry, 0, source));
	assert(!station_rx_authorized(entry, 1, source));

	uint8_t frame[26] = { 0 };
	uint32_t tags, length;

	for (tags = 0; tags <= 3; tags++) {
		memset(frame, 0, sizeof(frame));
		for (uint32_t i = 0; i < tags; i++) {
			frame[12 + i * 4] = i ? 0x81 : 0x88;
			frame[13 + i * 4] = i ? 0x00 : 0xa8;
		}
		length = 14 + tags * 4;
		frame[length - 2] = 0x08;
		check(frame, length, tags <= 2);
		for (uint32_t i = 0; i < length; i++)
			check(frame, i, 0);
		frame[length - 2] = 0x88;
		frame[length - 1] = 0x8e;
		check(frame, length, 0);
	}
	memset(frame, 0, sizeof(frame));
	frame[12] = 0x05;
	frame[13] = 0xff;
	check(frame, 14, 0);
	puts("PASS: station identity, MLO RX bands, revocation, EAPOL, VLAN and truncated headers");
	return 0;
}
