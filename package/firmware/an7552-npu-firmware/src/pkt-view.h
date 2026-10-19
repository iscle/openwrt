/* SPDX-License-Identifier: MIT */
#ifndef AN7552_PKT_VIEW_H
#define AN7552_PKT_VIEW_H
#include <stdint.h>

/* Packet buffers are uncached (~80 cycles per load) while the stack is
 * cached (~10). Load each header word at most once per frame.
 */
#define PKT_VIEW_WORDS 96U
struct pkt_view {
	const volatile uint8_t *base;
	uint32_t length;
	uint32_t valid[PKT_VIEW_WORDS / 32];
	uint32_t word[PKT_VIEW_WORDS];
};

static inline void pv_init(struct pkt_view *v, const volatile uint8_t *base,
			   uint32_t length)
{
	v->base = base;
	v->length = length;
	for (uint32_t i = 0; i < PKT_VIEW_WORDS / 32; i++)
		v->valid[i] = 0;
}

/* Never load past the frame: the tail word is assembled bytewise. */
static inline uint32_t pv_load(struct pkt_view *v, uint32_t index)
{
	uint32_t offset = index * 4, value = 0;

	if (offset + 4 <= v->length && !((uintptr_t)v->base & 3))
		return *(const volatile uint32_t *)(v->base + offset);
	for (uint32_t i = 0; i < 4 && offset + i < v->length; i++)
		value |= (uint32_t)v->base[offset + i] << (i * 8);
	return value;
}

static inline uint32_t pv_word(struct pkt_view *v, uint32_t index)
{
	uint32_t bit = 1U << (index & 31);

	if (index >= PKT_VIEW_WORDS)
		return pv_load(v, index);
	if (!(v->valid[index >> 5] & bit)) {
		v->word[index] = pv_load(v, index);
		v->valid[index >> 5] |= bit;
	}
	return v->word[index];
}

static inline uint32_t pv_byte(struct pkt_view *v, uint32_t offset)
{
	return (pv_word(v, offset >> 2) >> ((offset & 3) * 8)) & 0xff;
}

static inline uint32_t pv_get32(struct pkt_view *v, uint32_t offset)
{
	uint32_t shift = (offset & 3) * 8;

	if (!shift)
		return pv_word(v, offset >> 2);
	return (pv_word(v, offset >> 2) >> shift) |
	       (pv_word(v, (offset >> 2) + 1) << (32 - shift));
}
#endif
