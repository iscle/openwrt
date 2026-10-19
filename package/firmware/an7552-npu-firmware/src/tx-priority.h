/* SPDX-License-Identifier: MIT */
#ifndef AN7552_TX_PRIORITY_H
#define AN7552_TX_PRIORITY_H

/* The host supplies the same DSCP map used by mac80211 and the radio.
 * Inspect each packet: a bound flow need not retain its original DSCP.
 */
static int packet_priority(volatile uint8_t *packet, uint32_t length,
			   volatile uint8_t *map, uint32_t *priority)
{
	uint32_t protocol, offset = 14, vlan_priority = 0, dscp;

	if (length < offset)
		return 0;
	protocol = (uint32_t)packet[12] << 8 | packet[13];
	for (uint32_t tags = 0; protocol == 0x8100 || protocol == 0x88a8;
	     tags++) {
		if (tags == 2 || length - offset < 4)
			return 0;
		if (!vlan_priority)
			vlan_priority = packet[offset] >> 5;
		protocol = (uint32_t)packet[offset + 2] << 8 |
			   packet[offset + 3];
		offset += 4;
	}
	if (vlan_priority) {
		*priority = vlan_priority;
		return 1;
	}
	if (protocol == 0x0800) {
		uint32_t ihl;

		if (length - offset < 20 || packet[offset] >> 4 != 4)
			return 0;
		ihl = (packet[offset] & 15) * 4;
		if (ihl < 20 || length - offset < ihl)
			return 0;
		dscp = packet[offset + 1] >> 2;
	} else if (protocol == 0x86dd) {
		if (length - offset < 40 || packet[offset] >> 4 != 6)
			return 0;
		dscp = ((packet[offset] & 15) << 2) | (packet[offset + 1] >> 6);
	} else {
		*priority = protocol == 0x8917 ? 7 : 0;
		if (protocol == 0x8847 || protocol == 0x8848) {
			if (length - offset < 4)
				return 0;
			*priority = (packet[offset + 2] >> 1) & 7;
		}
		return 1;
	}
	*priority = map[dscp];
	return *priority < 8;
}

#endif
