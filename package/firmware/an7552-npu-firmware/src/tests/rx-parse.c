/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../rx-parse.h"
static void put32(uint8_t *p, uint32_t n)
{
	p[0] = n;
	p[1] = n >> 8;
	p[2] = n >> 16;
	p[3] = n >> 24;
}
int main(void)
{
	uint8_t p[256] = { 0 };
	struct rx_metadata m;
	put32(p, 0x10000000 | 160);
	put32(p + 4, 0x800f0002);
	put32(p + 8, 0x00040080);
	put32(p + 12, 1U << 16);
	put32(p + 32, 0x4188);
	put32(p + 40, (5U << 16) | 0x1230);
	put32(p + 48, 0x87654321);
	p[52] = 0xab;
	p[53] = 0xcd;
	p[96] = 2;
	p[108] = 8;
	p[110] = 0x45;
	p[113] = 50;
	p[119] = 6;
	p[142] = 0x50;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_OK);
	assert(m.offset == 96 && m.length == 64 && m.wcid == 2 && m.tid == 5);
	assert(m.pn_low == 0x87654321 && m.pn_high == 0xcdab &&
	       m.sequence == 0x123);
	for (unsigned int cipher = 0; cipher < 32; cipher++) {
		put32(p + 8, (cipher << 16) | 0x80);
		assert(rx_parse(p, 160, 0, &m) ==
		       ((cipher == 4 || cipher == 10 || cipher == 11 ||
			 cipher == 12) ?
				RX_PARSE_OK :
				RX_PARSE_SECURITY));
	}
	put32(p + 8, 0x00040080);
	for (unsigned int n = 0; n < 160; n++)
		assert(rx_parse(p, n, 0, &m) != RX_PARSE_OK);
	assert(rx_parse(p, 160, 1, &m) == RX_PARSE_TYPE);
	assert(rx_parse(p, 160, 2, &m) == RX_PARSE_TYPE);
	p[7] |= 2;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_SECURITY);
	p[7] &= ~2;
	p[33] |= 4;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_FRAGMENT);
	p[33] &= ~4;
	p[40] |= 1;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_FRAGMENT);
	p[40] &= ~1;
	p[16] = 1;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_OK);
	p[16] = 0;
	p[96] = 1;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_PROTOCOL);
	p[96] = 2;
	p[108] = 0x88;
	p[109] = 0x8e;
	assert(rx_parse(p, 160, 0, &m) == RX_PARSE_PROTOCOL);
	long page = sysconf(_SC_PAGESIZE);
	uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(guard != MAP_FAILED && !mprotect(guard + page, page, PROT_NONE));
	for (unsigned int n = 0; n < 256; n++) {
		uint8_t *at = guard + page - n;
		memcpy(at, p, n);
		if (n >= 4)
			put32(at, 0x10000000 | n);
		(void)rx_parse(at, n, 0, &m);
	}
	munmap(guard, page * 2);
	puts("PASS: RX metadata, cipher/error/fragment/control exclusions and guarded truncation");
}
