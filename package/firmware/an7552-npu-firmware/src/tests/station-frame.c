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
	(void)station_learn;
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
	puts("PASS: data, EAPOL, VLAN, nested VLAN and truncated headers");
	return 0;
}
