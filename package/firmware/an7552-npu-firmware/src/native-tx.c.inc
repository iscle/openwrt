/* Native mt76 host-ring consumer. Linux retains token completion ownership. */
static void native_transmit(uint32_t band)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL +
				    band * 64);
	volatile uint32_t *host, *txwi, *descriptor;
	uint32_t regs = band ? 0x200d8450 : 0x200d4420;
	uint32_t producer, next, consumer, count, base, control, token, i, len;
	uint32_t hardware_base = AN7552_NATIVE_RING_HOST + band * 0x8000;

	if (!q[5]) {
		q[6] = 0;
		return;
	}
	q[6] = 1;
	if (q[7])
		return;
	consumer = q[3];
	if (consumer == q[2])
		return;
	barrier();
	count = q[1];
	base = q[0];
	if (!count || count > 4096 || consumer >= count || (base & 3) ||
	    base < 0x80000000 || base > 0xa0000000 - count * 208) {
		q[7] = 1;
		return;
	}
	producer = q[4];
	next = (producer + 1) % AN7552_NATIVE_RING_COUNT;
	if (next == read32(regs + 12))
		return;
	host = (void *)(uintptr_t)(((base & 0x3fffffff) | 0x40000000) +
				   consumer * 208);
	control = host[0];
	len = (control >> 1) & 0x1ffff;
	if (!(control & 1) || len < 76 || len > 192 || host[1] < 0x80000000 ||
	    host[1] >= 0xa0000000) {
		q[7] = 2;
		return;
	}
	/* MAC TXP (AddBA) has a different token location from firmware TXP. */
	token = (host[4 + 7] & (1U << 27)) ? (host[4 + 8] & 0x7fff) :
					     (host[4 + 8] >> 16);
	if (token >= AN7552_TXWI_TOKENS) {
		q[7] = 3;
		return;
	}
	txwi = (void *)(uintptr_t)(AN7552_TXWI_NPU + token * 128);
	for (i = 0; i < 19; i++)
		txwi[i] = host[4 + i];
	len = (txwi[7] & (1U << 27)) ? 64 : 76;
	descriptor =
		(void *)(uintptr_t)((hardware_base & 0x3fffffff) | 0x40000000);
	descriptor += producer * 4;
	descriptor[0] = AN7552_TXWI_HOST + token * 128;
	descriptor[2] = host[1];
	descriptor[3] = 0;
	barrier();
	if (!(txwi[8] & 1) && (txwi[9] >> 24) == 1 &&
	    ((control >> 18) & 0x1fff) >= 14) {
		volatile uint8_t *packet =
			(void *)(uintptr_t)((host[1] & 0x3fffffff) |
					    0x40000000);
		if (!(packet[0] & 1)) {
			q[9] = txwi[9];
			q[10] = packet[0] | ((uint32_t)packet[1] << 8) |
				((uint32_t)packet[2] << 16) |
				((uint32_t)packet[3] << 24);
			q[11] = packet[4] | ((uint32_t)packet[5] << 8);
			q[13] = txwi[8] & 0xffff;
			station_learn(band, txwi[9], txwi[8] & 0xffff, packet,
				      (control >> 18) & 0x1fff);
		}
	}
	descriptor[1] = (len << 16) | (1U << 14) | 72;
	barrier();
	*(volatile uint32_t *)(uintptr_t)(regs + 8) = next;
	q[4] = next;
	barrier();
	q[3] = (consumer + 1) % count;
	q[8]++;
}

/* Bounded batches share one hardware consumer read and one doorbell write. */
static void native_forward(uint32_t *consumer, uint32_t *active, uint32_t band)
{
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL +
				    band * 64);
	uint32_t registers = band ? 0x200d8450 : 0x200d4420;
	uint32_t hardware = AN7552_NATIVE_RING_HOST + band * 0x8000;
	uint32_t producer, hardware_consumer, submitted = 0;

	if (!forward[0] || !read32(AN7552_NPU_BASE + AN7552_TDMA_CONTROL) ||
	    (forward[1] != 2 && forward[1] != band)) {
		*active = 0;
		forward[14] &= ~(1U << band);
		return;
	}
	if (!*active) {
		*consumer = forward[1] == 2 ? forward[6 + band] : forward[6];
		*active = 1;
		forward[14] |= 1U << band;
	}
	if (!q[5] || q[7])
		return;
	producer = q[4];
	hardware_consumer = read32(registers + 12);
	for (uint32_t budget = 0; budget < 32; budget++) {
		uint32_t token = band * AN7552_TDMA_COUNT + *consumer;
		volatile uint32_t *work =
			(void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
					    token * 16);
		volatile uint32_t *station, *txwi, *descriptor;
		volatile uint8_t *payload;
		uint32_t wcid, next, packet;

		if (work[0] != 1)
			break;
		barrier();
		wcid = (work[2] >> 14) & 0x7ff;
		if (!work[1] || work[1] > AN7552_TDMA_BUFFER_SIZE ||
		    (forward[1] != 2 && wcid != forward[3]) ||
		    ((work[2] >> 25) & 1) != band) {
			forward[11]++;
			goto drop;
		}
		station = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
		payload = (void *)(uintptr_t)(AN7552_TDMA_BUFFER_NPU +
					      token * AN7552_TDMA_BUFFER_SIZE);
		if (work[1] < 14 || !station[0] ||
		    station[1] != (band | (work[3] << 8)) ||
		    (station[4] != 0x80 && station[4] != 0x88) ||
		    station[2] != packet_mac_low(payload) ||
		    station[3] != packet_mac_high(payload))
			goto drop;
		next = (producer + 1) % AN7552_NATIVE_RING_COUNT;
		if (next == hardware_consumer)
			break;
		packet = AN7552_TDMA_BUFFER_HOST +
			 token * AN7552_TDMA_BUFFER_SIZE;
		txwi = (void *)(uintptr_t)(AN7552_TXWI_NPU + token * 128);
		for (uint32_t i = 0; i < 19; i++)
			txwi[i] = 0;
		txwi[8] = (token << 16) | station[4];
		txwi[9] = work[3] | (wcid << 8) | (1U << 24);
		txwi[10] = packet;
		txwi[16] = work[1];
		descriptor = (void *)(uintptr_t)(((hardware & 0x3fffffff) |
						  0x40000000) +
						 producer * 16);
		descriptor[0] = AN7552_TXWI_HOST + token * 128;
		descriptor[2] = packet;
		descriptor[3] = 0;
		barrier();
		descriptor[1] = (76U << 16) | (1U << 14) | 72;
		work[0] = 2;
		producer = next;
		q[12]++;
		forward[10]++;
		submitted++;
		*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
		continue;
drop:
		work[0] = 3;
		forward[12]++;
		*consumer = (*consumer + 1) % AN7552_TDMA_COUNT;
	}
	if (submitted) {
		barrier();
		*(volatile uint32_t *)(uintptr_t)(registers + 8) = producer;
		q[4] = producer;
	}
	forward[15] = (forward[15] & ~(0xffffU << (band * 16))) |
		      (*consumer << (band * 16));
}
