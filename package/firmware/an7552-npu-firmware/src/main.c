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
#include "tx-pool.c.inc"
#include "station.c.inc"
#include "native-tx.c.inc"
#include "txfree.c.inc"
#include "pn.c.inc"
#include "rx-security.c.inc"
#include "rx-parse.h"
#include "rro.c.inc"
#include "ppe.c.inc"
#include "upsplit.c.inc"

/* Timer 0 is reserved by the host for recovering unresponsive cores.
 * The PLIC is core-local; leave every unrelated source masked.
 */
static void recovery_interrupt_init(uint32_t core)
{
	for (uint32_t i = 0; i < 7; i++) {
		*(volatile uint32_t *)(uintptr_t)(0x0c002000 + 4 * i) = 0;
		*(volatile uint32_t *)(uintptr_t)(0x0c003000 + 4 * i) = ~0U;
	}
	*(volatile uint32_t *)(uintptr_t)0x0c00004c = 16;
	*(volatile uint32_t *)(uintptr_t)0x0c200000 = 0;
	*(volatile uint32_t *)(uintptr_t)0x0c003000 = ~(1U << 19);
	*(volatile uint32_t *)(uintptr_t)0x0c002000 = 1U << 19;
	barrier();
	__asm__ volatile("csrw mie, %0; csrsi mstatus, 8"
			 :
			 : "r"(1U << 11)
			 : "memory");
	*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
					  AN7552_FW_RECOVERY_READY + core * 4) =
		1;
	barrier();
}

void firmware_main(uint32_t core)
{
	volatile uint32_t *state =
		(void *)(uintptr_t)(AN7552_NPU_BASE + core * AN7552_STRIDE);
	uint32_t heartbeat = 0;
	struct up_front upload = { 0 };
	struct tx_band download[2] = { 0 };
	volatile uint32_t *tdma_state =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_STRIDE);
	if (!core)
		tx_pool_boot();

	up_init(core);
	state[1] = 0;
	state[2] = 0;
	state[3] = 0;
	state[4] = 0;
	*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
					  AN7552_FW_RUNTIME_ABI + core * 4) =
		AN7552_FW_ABI_VERSION;
	barrier();
	state[0] = AN7552_MAGIC;
	recovery_interrupt_init(core);
	for (;;) {
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
			if (read32(AN7552_NPU_BASE + AN7552_STATION_CONTROL)) {
				up_back_stopped();
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
			native_up_service();

			continue;
		}
		native_txfree(0);
		native_txfree(1);
		if (read32(AN7552_NPU_BASE + AN7552_STATION_CONTROL)) {
			up_front_pause();
			barrier();
			*(volatile uint32_t
				  *)(uintptr_t)(AN7552_NPU_BASE +
						AN7552_STATION_CONTROL + 4) = 1;
			continue;
		}
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
						  AN7552_STATION_CONTROL + 4) =
			0;
		up_pn_service();
		native_transmit(0);
		native_transmit(1);
		for (uint32_t band = 0; band < 2; band++)
			native_download(tdma_state, &download[band], band);
		native_up_front(&upload);
	}
}
