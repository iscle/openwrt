/* Source-NPU station identity validation; never learn authorization from data. */
static uint32_t packet_mac_low(volatile uint8_t *packet)
{
	if (!((uintptr_t)packet & 3))
		return *(const volatile uint32_t *)packet;
	return packet[0] | ((uint32_t)packet[1] << 8) |
	       ((uint32_t)packet[2] << 16) | ((uint32_t)packet[3] << 24);
}

static uint32_t packet_mac_high(volatile uint8_t *packet)
{
	return packet[4] | ((uint32_t)packet[5] << 8);
}

/* A raw MLO WCID identifies a reorder context, not necessarily its RX band. */
static inline int station_rx_authorized(volatile uint32_t *entry, uint32_t band,
                                        volatile uint8_t *source)
{
	return band < 2 && (entry[0] & AN7552_STATION_AUTHORIZED) &&
	       ((entry[1] & 255) == band || (entry[0] & AN7552_STATION_MLO)) &&
	       entry[2] == packet_mac_low(source) &&
	       entry[3] == packet_mac_high(source);
}

static int station_data_frame(volatile uint8_t *packet, uint32_t length)
{
	uint32_t offset = 12, protocol;

	/* A tagged EAPOL frame must never teach us plaintext data policy. */
	for (uint32_t tags = 0; tags <= 2; tags++) {
		if (length < offset + 2)
			return 0;
		protocol = ((uint32_t)packet[offset] << 8) | packet[offset + 1];
		if (protocol != 0x8100 && protocol != 0x88a8)
			return protocol >= 0x0600 && protocol != 0x888e;
		offset += 4;
	}
	return 0;
}

static void station_learn(uint32_t band, uint32_t metadata, uint32_t flags,
			  volatile uint8_t *packet, uint32_t length)
{
	uint32_t wcid = (metadata >> 8) & 0xffff;
	volatile uint32_t *entry;

	if (wcid >= AN7552_STATION_COUNT || (flags != 0x80 && flags != 0x88))
		return;
	if (!station_data_frame(packet, length))
		return;
	entry = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
	/* Never let queued plaintext traffic replace host-installed key policy. */
	if ((entry[0] & AN7552_STATION_AUTHORIZED) && !entry[4] &&
	    entry[1] == (band | ((metadata & 255) << 8)) &&
	    entry[2] == packet_mac_low(packet) &&
	    entry[3] == packet_mac_high(packet))
		entry[4] = flags;
}
