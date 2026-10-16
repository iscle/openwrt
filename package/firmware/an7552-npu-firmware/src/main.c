/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include "protocol.h"
#include "firmware-abi.h"

static inline void barrier(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static uint32_t read32(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

#define TDMA_BATCH 32U
/* Reclaim and consume bounded batches with one doorbell write per batch.
 * The status words live in the core 1 state block, where Linux reads them
 * (the activity mask at word 12 in particular), whichever core runs this.
 */
static void tdma_receive(volatile uint32_t *state, uint32_t *consumer,
			 uint32_t *reclaim, uint32_t band)
{
	volatile uint32_t *enable =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *work = 0;
	volatile uint32_t *descriptor;
	uint32_t control, length, last = 0, returned = 0, forwarding;
	uint32_t registers = 0x1fb50900 + band * 16;

	if (!*enable) {
		*consumer = 0;
		*reclaim = 0;
		state[12] &= ~(1U << band);
		return;
	}
	if (!(state[12] & (1U << band))) {
		*consumer = read32(registers + 12) % AN7552_TDMA_COUNT;
		*reclaim = *consumer;
	}
	state[12] |= 1U << band;
	forwarding = forward[0] && (forward[1] == 2 || forward[1] == band);
	if (forwarding) {
		for (uint32_t n = 0; n < TDMA_BATCH; n++) {
			work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
						   (band * AN7552_TDMA_COUNT +
						    *reclaim) *
							   16);
			if (work[0] != 3)
				break;
			barrier();
			descriptor =
				(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
						    (band * AN7552_TDMA_COUNT +
						     *reclaim) *
							    32);
			descriptor[1] = AN7552_TDMA_BUFFER_SIZE;
			work[0] = 0;
			last = *reclaim;
			returned = 1;
			*reclaim = (*reclaim + 1) % AN7552_TDMA_COUNT;
			forward[9]++;
		}
	}
	for (uint32_t n = 0; n < TDMA_BATCH; n++) {
		if (forwarding) {
			work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
						   (band * AN7552_TDMA_COUNT +
						    *consumer) *
							   16);
			if (work[0])
				break;
		}
		descriptor =
			(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
					    (band * AN7552_TDMA_COUNT +
					     *consumer) *
						    32);
		control = descriptor[1];
		if (!(control & 0x80000000))
			break;
		barrier();
		length = control & 0xffff;
		if (!length || length > AN7552_TDMA_BUFFER_SIZE ||
		    descriptor[2] != AN7552_TDMA_BUFFER_HOST +
					     (band * AN7552_TDMA_COUNT +
					      *consumer) *
						     AN7552_TDMA_BUFFER_SIZE) {
			state[14]++;
			*enable = 0;
			break;
		}
		state[5]++;
		state[7] = length;
		if (forwarding) {
			work[1] = length;
			work[2] = descriptor[4];
			work[3] = descriptor[6] >> 24;
			barrier();
			work[0] = 1;
			forward[8]++;
			*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
			continue;
		}
		state[8] = descriptor[4];
		state[9] = descriptor[5];
		state[10] = descriptor[6];
		state[11] = descriptor[7];
		descriptor[1] = AN7552_TDMA_BUFFER_SIZE;
		last = *consumer;
		returned = 1;
		*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
	}
	if (returned) {
		barrier();
		*(volatile uint32_t *)(uintptr_t)(registers + 8) = last;
	}
}

#include "station.c.inc"
#include "native-tx.c.inc"
#include "txfree.c.inc"
#include "pn.c.inc"
#include "rx-security.c.inc"
#include "rx-parse.h"
#include "rro.c.inc"
#include "ppe.c.inc"
#include "prof.h"

void firmware_main(uint32_t core)
{
	volatile uint32_t *state =
		(void *)(uintptr_t)(AN7552_NPU_BASE + core * AN7552_STRIDE);
	/* TDMA status words 5..14 keep their Linux-visible home in the core 1
	 * block; core 1 itself writes only words 0..4 there.
	 */
	volatile uint32_t *tdma_state =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_STRIDE);
	uint32_t heartbeat = 0;
	uint32_t consumer[2] = { 0 }, reclaim[2] = { 0 };
	uint32_t direct_consumer[2] = { 0 }, direct_active[2] = { 0 };

	state[1] = 0;
	state[2] = 0;
	state[3] = 0;
	state[4] = 0;
	*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
					  AN7552_FW_RUNTIME_ABI + core * 4) =
		AN7552_FW_ABI_VERSION;
	barrier();
	state[0] = AN7552_MAGIC;
	PROF_DECLARE(prof, core);
	for (;;) {
		PROF_TICK(prof, core);
		if (read32(AN7552_NPU_BASE + AN7552_PARK_CONTROL)) {
			barrier();
			*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
							  AN7552_PARK_ACK +
							  core * 4) =
				AN7552_PARK_MAGIC;
			barrier();
			for (;;)
				__asm__ volatile("wfi");
		}
		state[1] = ++heartbeat;
		if (core) {
			PROF(prof, 1, native_pn_service());
			PROF(prof, 2, native_ppe_service());
			if (read32(AN7552_NPU_BASE + AN7552_STATION_CONTROL)) {
				barrier();
				*(volatile uint32_t
					  *)(uintptr_t)(AN7552_NPU_BASE +
							AN7552_STATION_CONTROL +
							8) = 1;
				continue;
			}
			*(volatile uint32_t
				  *)(uintptr_t)(AN7552_NPU_BASE +
						AN7552_STATION_CONTROL + 8) = 0;
			/* Core 1 owns the whole Wi-Fi -> Ethernet direction. */
			PROF(prof, 3, native_rro_receive());
			continue;
		}
		PROF(prof, 1, native_txfree(0); native_txfree(1));
		if (read32(AN7552_NPU_BASE + AN7552_STATION_CONTROL)) {
			barrier();
			*(volatile uint32_t
				  *)(uintptr_t)(AN7552_NPU_BASE +
						AN7552_STATION_CONTROL + 4) = 1;
			continue;
		}
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
						  AN7552_STATION_CONTROL + 4) =
			0;
		PROF(prof, 2, native_transmit(0); native_transmit(1));
		/* Core 0 owns the whole Ethernet -> Wi-Fi direction: TDMA
		 * ingress, direct transmit and completion reclaim.
		 */
		for (uint32_t band = 0; band < 2; band++) {
			PROF(prof, 3,
			     tdma_receive(tdma_state, &consumer[band],
					  &reclaim[band], band));
			barrier();
			*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
							  AN7552_TDMA_CONTROL +
							  4 + band * 4) =
				consumer[band];
			PROF(prof, 4,
			     native_forward(&direct_consumer[band],
					    &direct_active[band], band));
		}
	}
}
