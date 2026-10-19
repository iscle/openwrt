/* SPDX-License-Identifier: MIT */
/* Exercise the actual PN/backend exclusion protocol at its fence boundaries.
 * No hardware timing assumptions: the callback runs the other hart exactly
 * while one side has published intent but has not checked its peer yet.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../protocol.h"

static void (*at_fence)(void);
static void barrier(void)
{
	void (*callback)(void) = at_fence;

	__sync_synchronize();
	at_fence = NULL;
	if (callback)
		callback();
}
static uint32_t read32(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}
#include "../pn.c.inc"
#include "../rx-security.c.inc"
#include "../rx-parse.h"
#include "../station.c.inc"
#include "../rro.c.inc"
#include "../ppe.c.inc"
#include "../upsplit.c.inc"

static volatile uint32_t *mailbox(void)
{
	return (void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
}
static void command(uint32_t sequence, uint32_t operation)
{
	volatile uint32_t *q = mailbox();

	q[1] = operation;
	q[2] = 7;
	q[3] = 0;
	q[6] = operation == 2 ? 1 : 0;
	q[0] = sequence;
}
static void command_during_entry(void)
{
	command(2, 2);
	up_pn_service();
	assert(mailbox()[8] == 1);
}
static void entry_during_command(void)
{
	assert(up_ctrl()[US_PN_PAUSE]);
	assert(!up_back_enter());
}

int main(void)
{
	(void)station_learn;
	assert(mmap((void *)0x3e900000, 0x4000, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
	       (void *)0x3e900000);
	assert(mmap((void *)0x44100000, 0x300000, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
	       (void *)0x44100000);
	up_init(0);
	up_init(1);

	/* An update cannot complete while the back end may submit an old frame. */
	assert(up_back_enter());
	command(1, 2);
	up_pn_service();
	assert(mailbox()[8] == 0 && !pn_entry(7)[0]);
	assert(up_ctrl()[US_PN_PAUSE] && up_ctrl()[US_BACK_ACTIVE]);
	up_back_exit();
	up_pn_service();
	assert(mailbox()[8] == 1 && pn_entry(7)[0] == 1);
	assert(pn_entry(7)[1] == 1 && !up_ctrl()[US_PN_PAUSE]);

	/* Intent crosses in both directions: exactly one side can enter. */
	at_fence = command_during_entry;
	assert(!up_back_enter());
	assert(!up_ctrl()[US_BACK_ACTIVE] && mailbox()[8] == 1);
	up_pn_service();
	assert(mailbox()[8] == 2 && pn_entry(7)[1] == 2);
	command(3, 3); /* Snapshot revokes RX eligibility. */
	at_fence = entry_during_command;
	up_pn_service();
	assert(mailbox()[8] == 3 && !pn_entry(7)[0]);
	assert(!up_ctrl()[US_PN_PAUSE] && !up_ctrl()[US_BACK_ACTIVE]);
	assert(up_back_enter());
	up_back_exit();

	/* RRO stop must finish even while both cores honor station quiescence. */
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	up_ctrl()[US_HEAD] = 12;
	up_ctrl()[US_TAIL] = 3;
	up_ctrl()[US_ACTIVE] = 1;
	up_ctrl()[US_ENABLE] = 1;
	q[5] = 1;
	assert(up_back_stopped() && q[5]);
	up_front_pause();
	assert(up_back_stopped() && !q[5]);
	assert(!up_ctrl()[US_ACTIVE] && !up_ctrl()[US_ENABLE]);
	assert(up_ctrl()[US_HEAD] == up_ctrl()[US_TAIL]);
	assert(up_ctrl()[US_DROPPED] == 9);
	puts("PASS: PN/backend mutual exclusion, fenced entry races, snapshot revoke and station-pause stop");
	return 0;
}
