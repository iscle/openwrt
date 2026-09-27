/* SPDX-License-Identifier: MIT */
/* Mirror the driver's replay domain before separating fast and slow delivery. */
static uint32_t native_rx_security(volatile uint8_t *packet, uint32_t length,
				   uint32_t *cookie)
{
	uint32_t d0, d1, d2, d4, offset = 32, fc = 0, seq = 0, tid = 0, wcid,
				 low, high, result, aggregate;
	volatile uint32_t *entry;
	if (length < 32)
		return 0;
	d0 = packet_word(packet);
	d1 = packet_word(packet + 4);
	d2 = packet_word(packet + 8);
	d4 = packet_word(packet + 16);
	if ((d0 >> 27) != 2 || ((d2 >> 16) & 31) != 4 ||
	    (d1 & ((1U << 23) | (1U << 24) | (1U << 25))) || !(d1 & (1U << 16)))
		return 0;
	wcid = d1 & 4095;
	if (wcid >= AN7552_STATION_COUNT)
		return 0;
	entry = pn_entry(wcid);
	if (!entry[0] || read32(AN7552_NPU_BASE + PN_CONTROL + 36))
		return 0;
	*cookie = entry[1];
	if (((d1 >> 21) & 3) != ((entry[0] >> 8) & 3))
		return (wcid << 4) | 2;
	if (d1 & (1U << 19)) {
		if (length < 48)
			return 0;
		fc = packet_word(packet + 32) & 65535;
		seq = packet_word(packet + 40);
		tid = (seq >> 16) & 15;
		seq &= 65535;
		offset += 16;
	}
	if (length < offset + 16)
		return 0;
	/* Later 802.11 fragments remain under mac80211's fragment replay checks. */
	if (!(d2 & (1U << 7)) && (d2 & (1U << 27)) && (seq & 15))
		return 0;
	if (!(d2 & (1U << 7)) && !(fc & 0x0c) && !(fc & 0x100))
		tid = 16;
	low = packet_word(packet + offset);
	high = packet[offset + 4] | (uint32_t)packet[offset + 5] << 8;
	aggregate = d4 & 3;
	if (aggregate == 3 && (d2 & (1U << 7))) {
		uint32_t ethernet = offset + 16;
		if (d1 & (1U << 17))
			ethernet += 16;
		if (d1 & (1U << 18)) {
			ethernet += 16;
			if (d1 & (1U << 20))
				ethernet += 96;
		}
		ethernet += 2 * ((d2 >> 13) & 7);
		if (ethernet > length || length - ethernet < 14 ||
		    (packet[ethernet] == 0xaa && packet[ethernet + 1] == 0xaa &&
		     packet[ethernet + 2] == 3 && !packet[ethernet + 3] &&
		     !packet[ethernet + 4] && !packet[ethernet + 5])) {
			entry[40 + tid] = 0;
			return (wcid << 4) | 2;
		}
	}
	if (aggregate && aggregate != 3) {
		result = (entry[40 + tid] == (0x10000 | (seq >> 4)) &&
			  entry[4 + tid * 2] == low &&
			  entry[5 + tid * 2] == high) ?
				 1 :
				 0;
		if (aggregate == 1)
			entry[40 + tid] = 0;
	} else {
		result = pn_compare(wcid, tid, low, high);
		if (aggregate == 3 && result == 1)
			entry[40 + tid] = 0x10000 | (seq >> 4);
	}
	*cookie = entry[1];
	return (wcid << 4) | (result == 1 ? 1 : 2);
}
