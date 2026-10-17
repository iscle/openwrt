/* SPDX-License-Identifier: MIT */
#define TDMA_BATCH 32U

/* Direct transmit buffers: the 2048 ring buffers armed by Linux plus the
 * unused tail of the packet workspace up to the TXWI area.
 */
#define TX_POOL_BUFFERS 3040U
#define TX_BUFFER_HOST(id) (AN7552_TDMA_BUFFER_HOST + (id) * AN7552_TDMA_BUFFER_SIZE)
#define TX_BUFFER_NPU(id) (AN7552_TDMA_BUFFER_NPU + (id) * AN7552_TDMA_BUFFER_SIZE)

static int tdma_buffer_valid(uint32_t address)
{
	uint32_t offset = address - AN7552_TDMA_BUFFER_HOST;

	return address >= AN7552_TDMA_BUFFER_HOST &&
	       !(offset & (AN7552_TDMA_BUFFER_SIZE - 1)) &&
	       offset / AN7552_TDMA_BUFFER_SIZE < TX_POOL_BUFFERS;
}

/* Without forwarding, core 0 re-arms received slots in place. With forwarding
 * enabled, native_forward() consumes the TDMA ring itself (see native-tx.c.inc)
 * and this function is not called. Status words stay in the core 1 state
 * block, where Linux reads them.
 */
static void tdma_receive(volatile uint32_t *state, uint32_t *consumer,
			 uint32_t *reclaim, uint32_t band)
{
	volatile uint32_t *enable =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	volatile uint32_t *descriptor;
	uint32_t control, length, last = 0, returned = 0;
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
	for (uint32_t n = 0; n < TDMA_BATCH; n++) {
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
		    !tdma_buffer_valid(descriptor[2])) {
			state[14]++;
			*enable = 0;
			break;
		}
		state[5]++;
		state[7] = length;
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
