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

#include "tdma.c.inc"
#include "station.c.inc"
#include "tx-pool.c.inc"
#include "native-tx.c.inc"
#include "txfree.c.inc"
#include "pn.c.inc"
#include "rx-security.c.inc"
#include "rx-parse.h"
#include "rro.c.inc"
#include "ppe.c.inc"
#include "upsplit.c.inc"
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
	struct tx_band download[2] = { { 0 } };
	struct up_front upload = { 0 };

	state[1] = 0;
	state[2] = 0;
	state[3] = 0;
	state[4] = 0;
	*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
					  AN7552_FW_RUNTIME_ABI + core * 4) =
		AN7552_FW_ABI_VERSION;
	if (!core)
		tx_pool_boot();
	up_init(core);
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
			/* PPE return pops and the single-submission mode. */
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
			/* Back half of the Wi-Fi -> Ethernet direction: buffer
			 * ownership, PPE input, delivery to Linux, refill.
			 */
			PROF(prof, 3, native_up_back());
			continue;
		}
		PROF(prof, 1, native_txfree(0); native_txfree(1));
		/* Front half of the Wi-Fi -> Ethernet direction: replay
		 * service, then at most UP_FRONT_BUDGET frames, then download.
		 * It never reads the station table, so it also runs during a
		 * station quiesce and an RRO stop never waits on that.
		 */
		PROF(prof, 6, up_pn_service());
		PROF(prof, 5, native_up_front(&upload));
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
		 * ingress, direct transmit and completion reclaim. With
		 * forwarding the TDMA ring is consumed by native_forward_ring()
		 * and re-armed from the buffer pool.
		 */
		for (uint32_t band = 0; band < 2; band++)
			PROF(prof, 3,
			     native_download(tdma_state, &download[band], band));
	}
}
