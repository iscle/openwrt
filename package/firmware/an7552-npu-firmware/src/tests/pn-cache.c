/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../protocol.h"
static void barrier(void) { __sync_synchronize(); }
#include "../pn.c.inc"

int main(void)
{
	uint32_t initial[34] = { 0 }, snapshot[34];
	struct pn_cache c;
	const uint32_t address[] = { 0x44110000, 0x44300000 };
	for (unsigned i = 0; i < 2; i++)
		assert(mmap((void *)(uintptr_t)address[i], 0x100000,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
			MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)address[i]);
	(void)native_pn_service;
	assert(pn_install(2, 1, 0, initial));
	uint32_t domain = pn_entry(2)[2];
	assert(pn_install(3, 1, domain, initial));
	assert(pn_install(4, 1, 0, initial));
	pc_reset(&c);
	assert(pn_compare_cached(&c, 2, 5, 100, 0) == 1);
	/* Published immediately, even if recovery interrupts this batch. */
	assert(pn_domain_snapshot(domain, 0, snapshot) && snapshot[10] == 100);
	assert(pn_compare_cached(&c, 3, 5, 100, 0) == 0);
	assert(pn_compare_cached(&c, 3, 5, 101, 0) == 1);
	assert(pn_compare_cached(&c, 2, 5, 101, 0) == 0);
	assert(pn_compare_cached(&c, 4, 5, 1, 0) == 1);
	assert(pn_compare_cached(&c, 2, 4, 1, 0) == 1);
	assert(pn_compare_cached(&c, 2, 5, 101, 0) == 0);
	pn_entry(2)[0] = 0;
	assert(pn_compare_cached(&c, 2, 5, 102, 0) == 2);
	pn_entry(2)[0] = 1;
	assert(pn_compare_cached(&c, 2, 5, UINT32_MAX, 0) == 1);
	assert(pn_compare_cached(&c, 2, 5, 0, 1) == 1);
	assert(pn_domain_snapshot(domain, 0, snapshot));
	assert(snapshot[10] == 0 && snapshot[11] == 1);
	assert(pn_compare_cached(&c, 3, 5, UINT32_MAX, 0) == 0);
	assert(pn_compare_cached(&c, 2048, 5, 1, 0) == 3);
	assert(pn_compare_cached(&c, 2, 17, 1, 0) == 3);
	assert(pn_compare_cached(&c, 2, 5, 1, 65536) == 3);
	/* A key command ends the batch: reset prevents using its old domain. */
	initial[10] = 500;
	assert(pn_install(2, 1, 0, initial));
	pc_reset(&c);
	assert(pn_compare_cached(&c, 2, 5, 500, 0) == 0);
	assert(pn_compare_cached(&c, 2, 5, 501, 0) == 1);
	puts("PASS: PN batch cache preserves shared domains, immediate snapshots, revocation, TID isolation, wrap and key changes");
}
