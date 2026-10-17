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
		/* Linux may now reset the ring: forget the cached radio index. */
		tx_pool()->radio_producer[band] = TX_RADIO_STALE;
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

struct tx_band {
	uint32_t consumer, reclaim, active;
};

/* Forwarding mode: one pass over the TDMA ring of a band. Each received slot
 * is read once (control, buffer, info, metadata), validated and either queued
 * to the radio with a pool token or dropped; its buffer then belongs to the
 * radio or the pool, and tx_pool_arm() refills the slot from the pool.
 * The consumer published to Linux (TDMA_CONTROL + 4 + band * 4) is this
 * consumer. Shared counters are updated once per call.
 */
static void native_forward_ring(volatile uint32_t *tdma, struct tx_band *b,
				uint32_t band)
{
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL +
				    band * 64);
	volatile uint32_t *enable =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	struct tx_pool *pool = tx_pool();
	uint32_t registers = band ? 0x200d8450 : 0x200d4420;
	uint32_t ring = ((AN7552_NATIVE_RING_HOST + band * 0x8000) &
			 0x3fffffff) | 0x40000000;
	uint32_t consumer = b->consumer, producer, radio, mode, only;
	uint32_t received = 0, submitted = 0, dropped = 0, bad = 0, last = 0;
	uint32_t cached = TX_RADIO_STALE, st0 = 0, st1 = 0, st2 = 0, st3 = 0,
		 st4 = 0;

	if (!q[5] || q[7])
		goto refill;
	mode = forward[1];
	only = forward[3];
	producer = q[4];
	/* The radio consumer only moves forward, so a stale copy can only make
	 * the ring look fuller. It stays valid while nobody else moved the
	 * producer (native_transmit() or a ring reset by Linux).
	 */
	radio = pool->radio_consumer[band];
	if (pool->radio_producer[band] != producer) {
		radio = read32(registers + 12);
		pool->radio_producer[band] = producer;
	}
	for (uint32_t budget = 0; budget < TDMA_BATCH; budget++) {
		uint32_t index = band * AN7552_TDMA_COUNT + consumer;
		volatile uint32_t *slot =
			(void *)(uintptr_t)(AN7552_TDMA_RING_NPU + index * 32);
		volatile uint32_t *txwi, *descriptor, *payload;
		uint32_t control, address, info, meta, wcid, next, buffer, token;
		uint32_t length;

		/* Never consume the slot just before arm: slots [arm, consumer)
		 * hold stale descriptors, so the consumer must not wrap onto them.
		 */
		if ((consumer + 1) % AN7552_TDMA_COUNT == pool->arm[band])
			break;
		control = slot[1];
		if (!(control & 0x80000000))
			break;
		next = (producer + 1) % AN7552_NATIVE_RING_COUNT;
		if (next == radio) {
			radio = read32(registers + 12);
			if (next == radio)
				break;
		}
		if (!pool->free_tokens ||
		    pool->inflight[band] >= TX_INFLIGHT_CAP(band))
			break;
		barrier();
		length = control & 0xffff;
		address = slot[2];
		if (!length || length > AN7552_TDMA_BUFFER_SIZE ||
		    !tdma_buffer_valid(address)) {
			tdma[14]++;
			*enable = 0;
			break;
		}
		received++;
		last = length;
		buffer = pool->slot[band][consumer];
		if (address != TX_BUFFER_HOST(buffer)) {
			pool->errors++;
			bad++;
			goto drop;
		}
		info = slot[4];
		meta = slot[6] >> 24;
		wcid = (info >> 14) & 0x7ff;
		if ((mode != 2 && wcid != only) || ((info >> 25) & 1) != band) {
			bad++;
			goto drop;
		}
		if (length < 14)
			goto drop;
		if (wcid != cached) {
			/* Linux changes a station only after the quiesce
			 * handshake at the top of the core 0 loop.
			 */
			volatile uint32_t *station =
				(void *)(uintptr_t)(AN7552_STATION_NPU +
						    wcid * 32);

			st0 = station[0];
			st1 = station[1];
			st2 = station[2];
			st3 = station[3];
			st4 = station[4];
			cached = wcid;
		}
		if (!st0 || st1 != (band | (meta << 8)) ||
		    (st4 != 0x80 && st4 != 0x88))
			goto drop;
		/* Buffers are 2 KiB aligned: destination MAC is words 0 and 1. */
		payload = (void *)(uintptr_t)TX_BUFFER_NPU(buffer);
		if (st2 != payload[0] || st3 != (payload[1] & 0xffff))
			goto drop;
		token = pool->token[--pool->free_tokens];
		pool->token_buffer[token] = TX_TOKEN_VALID |
					    (band ? TX_TOKEN_BAND : 0) | buffer;
		pool->inflight[band]++;
		/* Firmware TXP: the TXD is left zero for the radio to build. */
		txwi = (void *)(uintptr_t)(AN7552_TXWI_NPU + token * 128);
		for (uint32_t i = 0; i < 19; i++)
			txwi[i] = 0;
		txwi[8] = (token << 16) | st4;
		txwi[9] = meta | (wcid << 8) | (1U << 24);
		txwi[10] = address;
		txwi[16] = length;
		descriptor = (void *)(uintptr_t)(ring + producer * 16);
		descriptor[0] = AN7552_TXWI_HOST + token * 128;
		descriptor[2] = address;
		descriptor[3] = 0;
		barrier();
		descriptor[1] = (76U << 16) | (1U << 14) | 72;
		producer = next;
		submitted++;
		consumer = (consumer + 1) % AN7552_TDMA_COUNT;
		continue;
drop:
		tx_pool_release_buffer(pool, buffer);
		dropped++;
		consumer = (consumer + 1) % AN7552_TDMA_COUNT;
	}
	pool->radio_consumer[band] = radio;
	if (submitted) {
		barrier();
		*(volatile uint32_t *)(uintptr_t)(registers + 8) = producer;
		q[4] = producer;
		pool->radio_producer[band] = producer;
		q[12] += submitted;
		forward[10] += submitted;
	}
	if (received) {
		/* Received counts reach Linux before the consumer that covers
		 * them, so "consumer caught up and [8] == [9]" means idle.
		 */
		tdma[5] += received;
		tdma[7] = last;
		forward[8] += received;
		if (bad)
			forward[11] += bad;
		if (dropped)
			forward[12] += dropped;
		tx_pool_publish(pool);
		b->consumer = consumer;
		forward[15] = (forward[15] & ~(0xffffU << (band * 16))) |
			      (consumer << (band * 16));
		barrier();
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
						  AN7552_TDMA_CONTROL + 4 +
						  band * 4) = consumer;
	}
refill:
	tx_pool_arm(band, b->consumer);
}

/* Core 0, Ethernet -> Wi-Fi for one band: TDMA ingress, direct transmit and
 * slot refill. Without forwarding the TDMA ring is re-armed in place.
 */
static void native_download(volatile uint32_t *tdma, struct tx_band *b,
			    uint32_t band)
{
	volatile uint32_t *forward =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_FORWARD_CONTROL);
	uint32_t mode = forward[1], bit = 1U << band;

	if (!forward[0] || !read32(AN7552_NPU_BASE + AN7552_TDMA_CONTROL) ||
	    (mode != 2 && mode != band)) {
		if (b->active) {
			tx_pool_stop(band);
			b->active = 0;
		}
		if (forward[14] & bit)
			forward[14] &= ~bit;
		tdma_receive(tdma, &b->consumer, &b->reclaim, band);
		barrier();
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
						  AN7552_TDMA_CONTROL + 4 +
						  band * 4) = b->consumer;
		return;
	}
	if (!b->active) {
		/* Linux armed every slot; the TDMA starts at the given index. */
		b->consumer = mode == 2 ? forward[6 + band] : forward[6];
		b->reclaim = b->consumer;
		b->active = 1;
		tx_pool_start(band, b->consumer);
		tdma[12] |= bit;
		forward[14] |= bit;
		forward[15] = (forward[15] & ~(0xffffU << (band * 16))) |
			      (b->consumer << (band * 16));
		barrier();
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
						  AN7552_TDMA_CONTROL + 4 +
						  band * 4) = b->consumer;
	}
	native_forward_ring(tdma, b, band);
}
