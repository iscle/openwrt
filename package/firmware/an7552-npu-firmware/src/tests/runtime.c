/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Exercise the production helpers without replacing the host C library. */
#define memset firmware_memset
#define memcpy firmware_memcpy
#define memmove firmware_memmove
#define memcmp firmware_memcmp
#include "../runtime.c"
#undef memset
#undef memcpy
#undef memmove
#undef memcmp

int main(void)
{
	unsigned char actual[128], expected[128], source[128];

	for (size_t i = 0; i < sizeof(source); i++)
		source[i] = (unsigned char)(i * 37);
	/* Misalignment, empty/small ranges and non-byte int conversion. */
	for (size_t offset = 0; offset < 16; offset++) {
		for (size_t length = 0; length <= 64; length++) {
			memset(actual, 0x5a, sizeof(actual));
			memset(expected, 0x5a, sizeof(expected));
			assert(firmware_memset(actual + offset, 0x1a5, length) ==
			       actual + offset);
			memset(expected + offset, 0x1a5, length);
			assert(!memcmp(actual, expected, sizeof(actual)));

			assert(firmware_memcpy(actual + offset, source + 15 - offset,
					       length) == actual + offset);
			memcpy(expected + offset, source + 15 - offset, length);
			assert(!memcmp(actual, expected, sizeof(actual)));
			assert(!firmware_memcmp(actual, expected, sizeof(actual)));
		}
	}
	/* Both overlap directions, identical pointers, and disjoint ranges. */
	for (size_t from = 0; from < 32; from++) {
		for (size_t to = 0; to < 32; to++) {
			for (size_t length = 0; length <= 64; length++) {
				memcpy(actual, source, sizeof(actual));
				memcpy(expected, source, sizeof(expected));
				assert(firmware_memmove(actual + to, actual + from,
							length) == actual + to);
				memmove(expected + to, expected + from, length);
				assert(!memcmp(actual, expected, sizeof(actual)));
			}
		}
	}
	/* Comparison uses unsigned bytes and stops at the first mismatch. */
	for (size_t i = 0; i < sizeof(actual); i++) {
		memset(actual, 0x7f, sizeof(actual));
		memcpy(expected, actual, sizeof(expected));
		expected[i] = 0x80;
		assert(firmware_memcmp(actual, expected, sizeof(actual)) < 0);
		assert(firmware_memcmp(expected, actual, sizeof(actual)) > 0);
		assert(!firmware_memcmp(actual, expected, i));
	}

	/* A helper must not access bytes past its range, including length zero. */
	long page = sysconf(_SC_PAGESIZE);
	assert(page > 0);
	unsigned char *mapping = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(mapping != MAP_FAILED);
	assert(!mprotect(mapping + page, page, PROT_NONE));
	unsigned char *end = mapping + page;
	assert(firmware_memset(end, 0, 0) == end);
	assert(firmware_memcpy(end, end, 0) == end);
	assert(firmware_memmove(end, end, 0) == end);
	assert(!firmware_memcmp(end, end, 0));
	for (size_t length = 1; length <= 64; length++) {
		assert(firmware_memset(end - length, -1, length) == end - length);
		assert(firmware_memcpy(actual, end - length, length) == actual);
		assert(!firmware_memcmp(actual, end - length, length));
		assert(firmware_memcpy(end - length, source, length) == end - length);
		assert(firmware_memmove(end - length, end - length - 1, length) == end - length);
		assert(firmware_memmove(end - length - 1, end - length, length) == end - length - 1);
	}
	assert(!munmap(mapping, page * 2));
	puts("freestanding memory runtime: ok");
	return 0;
}
