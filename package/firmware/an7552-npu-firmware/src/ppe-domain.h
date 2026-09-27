/* SPDX-License-Identifier: MIT */
#ifndef AN7552_PPE_DOMAIN_H
#define AN7552_PPE_DOMAIN_H

#include <stdint.h>

/* An internal C-tag separates stations sharing the PPE's Wi-Fi input port.
 * The host must remove the tag in every bound action and invalidate ingress
 * flows before reusing a station index. Tagged wireless frames use Linux.
 */
struct ppe_domain_saved {
	uint8_t bytes[16];
	uint32_t active;
};

static int ppe_domain_push(volatile uint8_t *buffer, uint32_t capacity,
			   uint32_t offset, uint32_t length, uint32_t domain,
			   volatile struct ppe_domain_saved *saved)
{
	volatile uint8_t *frame;
	uint32_t protocol;

	if (saved->active || !domain || domain > 2048 || offset < 4 ||
	    offset > capacity || length > capacity - offset || length < 14)
		return 0;
	frame = buffer + offset;
	protocol = (uint32_t)frame[12] << 8 | frame[13];
	if ((frame[0] & 1) || (protocol != 0x0800 && protocol != 0x86dd))
		return 0;

	/* The four bytes before the Ethernet header can belong to the RXD.
	 * Restore them too on a miss, including after host-ring backpressure.
	 */
	for (uint32_t i = 0; i < sizeof(saved->bytes); i++)
		saved->bytes[i] = buffer[offset - 4 + i];
	for (uint32_t i = 0; i < 12; i++)
		buffer[offset - 4 + i] = frame[i];
	frame[8] = 0x81;
	frame[9] = 0;
	frame[10] = domain >> 8;
	frame[11] = domain;
	saved->active = 1;
	return 1;
}

static void ppe_domain_restore(volatile uint8_t *buffer, uint32_t offset,
			       volatile struct ppe_domain_saved *saved)
{
	if (!saved->active)
		return;
	for (uint32_t i = 0; i < sizeof(saved->bytes); i++)
		buffer[offset - 4 + i] = saved->bytes[i];
	saved->active = 0;
}

#endif
