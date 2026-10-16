/* SPDX-License-Identifier: MIT */
#ifndef AN7552_PN_CACHE_H
#define AN7552_PN_CACHE_H
#include <stdint.h>

/* Batch-local copy of one station's replay entry (64 words). Words are
 * loaded on first use and dirty words are written back once by
 * pc_flush(). Word 0 (key installed) is never cached: Linux may clear it
 * at any time. Replay state otherwise changes only through core 1 PN
 * service requests, which run between receive batches.
 */
struct pn_cache {
	uint32_t wcid;
	uint32_t valid[2], dirty[2];
	uint32_t word[64];
};

static inline void pc_reset(struct pn_cache *c)
{
	c->wcid = 0xffffffff;
	c->valid[0] = c->valid[1] = 0;
	c->dirty[0] = c->dirty[1] = 0;
}

static inline void pc_flush(struct pn_cache *c)
{
	volatile uint32_t *entry;

	if (c->wcid == 0xffffffff || !(c->dirty[0] | c->dirty[1]))
		return;
	entry = pn_entry(c->wcid);
	for (uint32_t i = 1; i < 64; i++)
		if (c->dirty[i >> 5] & (1U << (i & 31)))
			entry[i] = c->word[i];
	c->dirty[0] = c->dirty[1] = 0;
}

static inline void pc_select(struct pn_cache *c, uint32_t wcid)
{
	if (c->wcid == wcid)
		return;
	pc_flush(c);
	c->wcid = wcid;
	c->valid[0] = c->valid[1] = 0;
}

static inline __attribute__((always_inline)) uint32_t pc_get(struct pn_cache *c, uint32_t i)
{
	uint32_t bit = 1U << (i & 31);

	if (!(c->valid[i >> 5] & bit)) {
		c->word[i] = pn_entry(c->wcid)[i];
		c->valid[i >> 5] |= bit;
	}
	return c->word[i];
}

static inline __attribute__((always_inline)) void pc_set(struct pn_cache *c, uint32_t i, uint32_t value)
{
	uint32_t bit = 1U << (i & 31);

	c->word[i] = value;
	c->valid[i >> 5] |= bit;
	c->dirty[i >> 5] |= bit;
}
#endif
