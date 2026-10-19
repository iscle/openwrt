/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../pkt-view.h"
int main(void)
{
	size_t page = sysconf(_SC_PAGESIZE);
	uint8_t *p = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct pkt_view v;
	assert(p != MAP_FAILED);
	assert(!mprotect(p + page, page, PROT_NONE));
	for (uint32_t length = 0; length <= 400; length++) {
		uint8_t *base = p + page - length;
		for (uint32_t i = 0; i < length; i++)
			base[i] = (uint8_t)(i * 7);
		pv_init(&v, base, length);
		for (uint32_t i = 0; i < length + 8; i++) {
			uint32_t expected = 0;
			for (uint32_t j = 0; j < 4 && i + j < length; j++)
				expected |= (uint32_t)base[i + j] << (8 * j);
			assert(pv_get32(&v, i) == expected);
		}
		assert(!pv_word(&v, UINT32_MAX));
		assert(!pv_get32(&v, UINT32_MAX));
	}
	p[0] = 1;
	pv_init(&v, p, 4);
	assert(pv_byte(&v, 0) == 1);
	p[0] = 2;
	assert(pv_byte(&v, 0) == 1);
	pv_init(&v, p, 4);
	assert(pv_byte(&v, 0) == 2);
	puts("PASS: bounded packet snapshot, aligned/unaligned tails, overflow and fresh token reuse");
}
