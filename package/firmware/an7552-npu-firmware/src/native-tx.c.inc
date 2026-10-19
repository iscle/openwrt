/* SPDX-License-Identifier: MIT */
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

#include "tx-priority.h"

/* Route through the same MLD identity; never borrow another peer's WCID. */
static int native_mlo_route(volatile uint32_t **station, uint32_t *wcid,
			    volatile uint8_t *packet, uint32_t length)
{
	volatile uint32_t *source = *station, *target, *qos;
	uint32_t priority, bss = source[1] >> 8, route, index;

	if (!(source[0] & AN7552_STATION_MLO))
		return 1;
	if (!source[7] || bss >= AN7552_QOS_COUNT)
		return 0;
	qos = (void *)(uintptr_t)(AN7552_QOS_NPU + bss * AN7552_QOS_STRIDE);
	if (!qos[16] ||
	    !packet_priority(packet, length, (void *)qos, &priority))
		return 0;
	route = source[5 + (priority & 1)];
	index = route & 65535;
	if (!index || index > AN7552_STATION_COUNT)
		return 0;
	target = (void *)(uintptr_t)(AN7552_STATION_NPU + (index - 1) * 32);
	if ((target[0] & (AN7552_STATION_AUTHORIZED | AN7552_STATION_MLO)) !=
		    (AN7552_STATION_AUTHORIZED | AN7552_STATION_MLO) ||
	    target[1] != route >> 16 || target[2] != source[2] ||
	    target[3] != source[3] || (target[4] != 0x80 && target[4] != 0x88))
		return 0;
	*wcid = index - 1;
	*station = target;
	return 1;
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
	volatile uint32_t *forward = tx_forward_control();
	volatile uint32_t *enable = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL);
	struct tx_pool *pool = tx_pool();
	volatile uint32_t *queues[2];
	const uint32_t registers[2] = { 0x200d4420, 0x200d8450 };
	uint32_t producer[2], radio[2], submitted[2] = { 0, 0 };
	uint32_t consumer = b->consumer, received = 0, dropped = 0, bad = 0, last = 0;

	for (uint32_t i = 0; i < 2; i++) {
		queues[i] = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_NATIVE_CONTROL + i * 64);
		producer[i] = queues[i][4];
		radio[i] = pool->radio_consumer[i];
		if (pool->radio_producer[i] != producer[i] && queues[i][5] && !queues[i][7])
			radio[i] = read32(registers[i] + 12);
	}
	for (uint32_t budget = 0; budget < TDMA_BATCH; budget++) {
		volatile uint32_t *slot = (void *)(uintptr_t)(AN7552_TDMA_RING_NPU +
			(band * AN7552_TDMA_COUNT + consumer) * 32);
		volatile uint32_t *station, *q, *descriptor, *txwi;
		volatile uint8_t *payload;
		uint32_t control, length, address, buffer, info, bss, wcid, target, next, token;

		if ((consumer + 1) % AN7552_TDMA_COUNT == pool->arm[band])
			break;
		control = slot[1];
		if (!(control & 0x80000000))
			break;
		barrier();
		length = control & 65535;
		address = slot[2];
		buffer = pool->slot[band][consumer];
		/* Never return an unknown/corrupted DMA address to the free pool. */
		if (!length || length > AN7552_TDMA_BUFFER_SIZE ||
		    buffer >= TX_POOL_BUFFERS || address != TX_BUFFER_HOST(buffer)) {
			tdma[14]++;
			pool->errors++;
			*enable = 0;
			break;
		}
		info = slot[4];
		bss = slot[6] >> 24;
		wcid = (info >> 14) & 0x7ff;
		if ((forward[1] != 2 && wcid != forward[3]) || ((info >> 25) & 1) != band) {
			bad++;
			goto drop;
		}
		if (!queues[band][5] || queues[band][7] || length < 14)
			goto drop;
		station = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
		payload = (void *)(uintptr_t)TX_BUFFER_NPU(buffer);
		if (!(station[0] & AN7552_STATION_AUTHORIZED) ||
		    station[1] != (band | (bss << 8)) ||
		    (station[4] != 0x80 && station[4] != 0x88) ||
		    station[2] != packet_mac_low(payload) || station[3] != packet_mac_high(payload) ||
		    !native_mlo_route(&station, &wcid, payload, length))
			goto drop;
		target = station[1] & 255;
		bss = station[1] >> 8;
		if (target > 1 || bss > 255)
			goto drop;
		q = queues[target];
		if (!q[5] || q[7])
			goto drop;
		next = (producer[target] + 1) % AN7552_NATIVE_RING_COUNT;
		if (next == radio[target]) {
			radio[target] = read32(registers[target] + 12);
			if (next == radio[target])
				break;
		}
		if (!pool->free_tokens || pool->inflight[target] >= TX_INFLIGHT_CAP(target))
			break;
		/* Authorization is live even when immutable identity fields are cached. */
		if (!(station[0] & AN7552_STATION_AUTHORIZED))
			goto drop;
		token = pool->token[--pool->free_tokens];
		pool->token_buffer[token] = TX_TOKEN_VALID | (target ? TX_TOKEN_BAND : 0) | buffer;
		pool->inflight[target]++;
		txwi = (void *)(uintptr_t)(AN7552_TXWI_NPU + token * 128);
		for (uint32_t i = 0; i < 19; i++)
			txwi[i] = 0;
		txwi[8] = (token << 16) | station[4];
		txwi[9] = bss | (wcid << 8) | (1U << 24);
		txwi[10] = address;
		txwi[16] = length;
		descriptor = (void *)(uintptr_t)(((AN7552_NATIVE_RING_HOST + target * 0x8000) &
			0x3fffffff) | 0x40000000);
		descriptor += producer[target] * 4;
		descriptor[0] = AN7552_TXWI_HOST + token * 128;
		descriptor[2] = address;
		descriptor[3] = 0;
		barrier();
		descriptor[1] = (76U << 16) | (1U << 14) | 72;
		producer[target] = next;
		submitted[target]++;
		goto consumed;
drop:
		tx_pool_release_buffer(pool, buffer);
		dropped++;
consumed:
		received++;
		last = length;
		consumer = (consumer + 1) % AN7552_TDMA_COUNT;
	}
	for (uint32_t i = 0; i < 2; i++) {
		if (!queues[i][5] || queues[i][7]) {
			pool->radio_producer[i] = TX_RADIO_STALE;
			continue;
		}
		pool->radio_consumer[i] = radio[i];
		pool->radio_producer[i] = producer[i];
		if (!submitted[i])
			continue;
		barrier();
		*(volatile uint32_t *)(uintptr_t)(registers[i] + 8) = producer[i];
		queues[i][4] = producer[i];
		queues[i][12] += submitted[i];
		forward[10] += submitted[i];
	}
	if (received) {
		tdma[5] += received;
		tdma[7] = last;
		forward[8] += received;
		forward[11] += bad;
		forward[12] += dropped;
		tx_pool_publish(pool);
		b->consumer = consumer;
		forward[15] = (forward[15] & ~(0xffffU << (band * 16))) | (consumer << (band * 16));
		barrier();
		*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TDMA_CONTROL + 4 + band * 4) = consumer;
	}
	if (*enable)
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
