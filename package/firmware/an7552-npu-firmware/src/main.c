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

static void tdma_receive(volatile uint32_t *state, uint32_t *consumer,
			 uint32_t *reclaim, uint32_t band)
{
	volatile uint32_t *enable =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *work;
	volatile uint32_t *descriptor;
	uint32_t control, length;
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
	if (forward[0] && (forward[1] == 2 || forward[1] == band)) {
		work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
					   (band * AN7552_TDMA_COUNT +
					    *reclaim) *
						   16);
		if (work[0] == 3) {
			barrier();
			descriptor =
				(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
						    (band * AN7552_TDMA_COUNT +
						     *reclaim) *
							    32);
			descriptor[1] = AN7552_TDMA_BUFFER_SIZE;
			work[0] = 0;
			barrier();
			*(volatile uint32_t *)(uintptr_t)(registers + 8) =
				*reclaim;
			*reclaim = (*reclaim + 1) % AN7552_TDMA_COUNT;
			forward[9]++;
		}
		work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
					   (band * AN7552_TDMA_COUNT +
					    *consumer) *
						   16);
		if (work[0])
			return;
	}
	descriptor =
		(void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
				    (band * AN7552_TDMA_COUNT + *consumer) *
					    32);
	control = descriptor[1];
	if (!(control & 0x80000000))
		return;
	barrier();
	length = control & 0xffff;
	if (!length || length > AN7552_TDMA_BUFFER_SIZE ||
	    descriptor[2] != AN7552_TDMA_BUFFER_HOST +
				     (band * AN7552_TDMA_COUNT + *consumer) *
					     AN7552_TDMA_BUFFER_SIZE) {
		state[14]++;
		*enable = 0;
		return;
	}
	state[5]++;
	state[7] = length;
	state[8] = descriptor[4];
	state[9] = descriptor[5];
	state[10] = descriptor[6];
	state[11] = descriptor[7];
	if (forward[0] && (forward[1] == 2 || forward[1] == band)) {
		work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
					   (band * AN7552_TDMA_COUNT +
					    *consumer) *
						   16);
		work[1] = length;
		work[2] = descriptor[4];
		work[3] = descriptor[6] >> 24;
		barrier();
		work[0] = 1;
		forward[8]++;
		*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
		return;
	}
	descriptor[1] = AN7552_TDMA_BUFFER_SIZE;
	barrier();
	*(volatile uint32_t *)(uintptr_t)(registers + 8) = *consumer;
	*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
}

#include "station.c.inc"
#include "native-tx.c.inc"
#include "txfree.c.inc"
#include "pn.c.inc"
#include "rx-security.c.inc"
#include "rx-parse.h"
#include "rro.c.inc"
#include "ppe.c.inc"

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
			native_pn_service();
			native_ppe_service();
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
			native_rro_receive();
			for (uint32_t band = 0; band < 2; band++) {
				tdma_receive(state, &consumer[band],
					     &reclaim[band], band);
				barrier();
				*(volatile uint32_t
					  *)(uintptr_t)(AN7552_NPU_BASE +
							AN7552_TDMA_CONTROL +
							4 + band * 4) =
					consumer[band];
			}
			continue;
		}
		native_txfree(0);
		native_txfree(1);
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
		native_transmit(0);
		native_transmit(1);
		for (uint32_t band = 0; band < 2; band++)
			native_forward(&direct_consumer[band],
				       &direct_active[band], band);
	}
}
