/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

/* Freestanding compilers can lower aggregate initialization and assignment to
 * these calls even with -fno-builtin. Volatile byte accesses keep the compiler
 * from recognizing the implementation itself as another memory-library call.
 * These helpers operate on ordinary RAM; shared DMA/MMIO uses explicit fences.
 */
void *memset(void *destination, int value, size_t length)
{
	volatile unsigned char *out = destination;

	while (length--)
		*out++ = (unsigned char)value;
	return destination;
}

void *memcpy(void *destination, const void *source, size_t length)
{
	volatile unsigned char *out = destination;
	const volatile unsigned char *in = source;

	while (length--)
		*out++ = *in++;
	return destination;
}

void *memmove(void *destination, const void *source, size_t length)
{
	volatile unsigned char *out = destination;
	const volatile unsigned char *in = source;

	if ((uintptr_t)destination < (uintptr_t)source) {
		while (length--)
			*out++ = *in++;
	} else {
		while (length) {
			length--;
			out[length] = in[length];
		}
	}
	return destination;
}

int memcmp(const void *first, const void *second, size_t length)
{
	const volatile unsigned char *a = first, *b = second;

	while (length--) {
		unsigned char left = *a++, right = *b++;

		if (left != right)
			return (int)left - (int)right;
	}
	return 0;
}
