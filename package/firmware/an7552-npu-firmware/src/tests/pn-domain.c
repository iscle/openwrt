/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static jmp_buf trap;
static unsigned int trap_after;
static void barrier(void)
{
	__sync_synchronize();
	if (trap_after && !--trap_after)
		longjmp(trap, 1);
}
#include "../protocol.h"
#include "../pn-domain.c.inc"

int main(void)
{
	uint32_t initial[34] = {}, saved[34], peer, other, replacement;
	uint32_t domains[PN_DOMAIN_COUNT];
	volatile uint32_t *slot;

	assert(mmap((void *)(uintptr_t)PN_DOMAIN_CORE, PN_DOMAIN_COUNT * 256,
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
		    MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)PN_DOMAIN_CORE);
	assert(!pn_domain_create(4, initial));
	assert(!pn_domain_create(0, NULL));
	initial[33] = 65536;
	assert(!pn_domain_create(0, initial));
	initial[33] = 0;
	peer = pn_domain_create(0, initial);
	assert(peer && pn_domain_attach(peer, 0, initial));
	other = pn_domain_create(0, initial);
	assert(other && peer != other);
	/* Both links share this handle; another peer remains independent. */
	assert(pn_domain_compare(peer, 0, 5, 100, 0) == 1);
	assert(pn_domain_compare(peer, 0, 5, 100, 0) == 0);
	assert(pn_domain_compare(other, 0, 5, 100, 0) == 1);
	assert(pn_domain_compare(peer, 0, 4, 100, 0) == 1);
	assert(pn_domain_compare(peer, 1, 5, 101, 0) == 3);
	assert(pn_domain_compare(peer, 0, 17, 101, 0) == 3);
	assert(pn_domain_compare(peer, 0, 5, 101, 65536) == 3);
	assert(!pn_domain_attach(peer, 1, initial));
	/* Removing the first-created link cannot free the surviving link's PN. */
	assert(pn_domain_release(peer, 0));
	assert(pn_domain_compare(peer, 0, 5, 100, 0) == 0);
	initial[10] = 90;
	assert(pn_domain_attach(peer, 0, initial));
	assert(pn_domain_compare(peer, 0, 5, 100, 0) == 0);
	initial[10] = 200;
	assert(pn_domain_attach(peer, 0, initial));
	assert(pn_domain_compare(peer, 0, 5, 200, 0) == 0);
	assert(pn_domain_snapshot(peer, 0, saved) && saved[10] == 200);
	assert(!pn_domain_snapshot(peer, 1, saved));
	assert(pn_domain_release(peer, 0));
	assert(pn_domain_release(peer, 0));
	assert(pn_domain_release(peer, 0));
	assert(!pn_domain_release(peer, 0));
	assert(pn_domain_compare(peer, 0, 5, 201, 0) == 3);
	replacement = pn_domain_create(0, initial);
	assert(replacement && replacement != peer);
	assert(!pn_domain_attach(peer, 0, initial));
	assert(!pn_domain_release(peer, 0));
	assert(pn_domain_compare(replacement, 0, 5, 201, 0) == 1);
	/* Check fail-closed low-word wrap at each publication boundary. */
	slot = pn_domain_lookup(replacement, 0);
	for (volatile unsigned int boundary = 1; boundary <= 2; boundary++) {
		slot[4] = UINT32_MAX;
		slot[5] = 0;
		trap_after = boundary;
		if (!setjmp(trap))
			(void)pn_domain_compare(replacement, 0, 0, 0, 1);
		trap_after = 0;
		assert(pn_domain_compare(replacement, 0, 0, UINT32_MAX, 0) == 0);
	}
	assert(pn_domain_release(replacement, 0));
	assert(pn_domain_release(other, 0));
	/* No active slot may be recycled when the pool is full. */
	for (unsigned int i = 0; i < PN_DOMAIN_COUNT; i++) {
		domains[i] = pn_domain_create(0, initial);
		assert(domains[i]);
	}
	assert(!pn_domain_create(0, initial));
	for (unsigned int i = 0; i < PN_DOMAIN_COUNT; i++)
		assert(pn_domain_release(domains[i], 0));
	/* Retire a slot whose generation cannot advance. */
	pn_domain_slot(0)[1] = PN_DOMAIN_GENERATION_MAX;
	replacement = pn_domain_create(0, initial);
	assert(replacement && (replacement & (PN_DOMAIN_COUNT - 1)) == 1);
	assert(pn_domain_release(replacement, 0));
	/* A recovery must preserve generations while revoking all references. */
	peer = pn_domain_create(0, initial);
	assert(peer);
	for (unsigned int i = 0; i < PN_DOMAIN_COUNT; i++)
		pn_domain_slot(i)[0] = 0;
	replacement = pn_domain_create(0, initial);
	assert(replacement && replacement != peer);
	assert(!pn_domain_attach(peer, 0, initial));
	assert(pn_domain_release(replacement, 0));
	puts("PASS: shared PN domains, peer/TID isolation, lifetime, reuse, bounds and fault boundaries");
}
