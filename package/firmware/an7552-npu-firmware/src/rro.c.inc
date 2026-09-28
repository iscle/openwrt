/* SPDX-License-Identifier: MIT */
#include "ppe-domain.h"
static volatile uint32_t *rx_words(uint32_t host)
{
	return (void *)(uintptr_t)((host & 0x3fffffff) | 0x40000000);
}

static void native_rx_refill(uint32_t band)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *data =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				    band * 64);
	volatile uint32_t *free_ids = rx_words(AN7552_RX_FREE_HOST);
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST);
	uint32_t producer = data[6], consumer = read32(data[4] + 12);

	data[8] = consumer;
	if (consumer >= AN7552_RX_RING_COUNT) {
		q[6] = 1;
		return;
	}
	for (uint32_t budget = 0; budget < 32; budget++) {
		volatile uint32_t *descriptor;
		uint32_t token, next = (producer + 1) % AN7552_RX_RING_COUNT;

		if (next == consumer || !q[22])
			break;
		token = free_ids[q[20]];
		if (token >= AN7552_RX_BUFFER_COUNT || states[token]) {
			q[6] = 2;
			return;
		}
		q[20] = (q[20] + 1) % AN7552_RX_BUFFER_COUNT;
		q[22]--;
		states[token] = 1 | (band << 8);
		descriptor = rx_words(AN7552_RX_DATA_RING_HOST + band * 0x8000 +
				      producer * 16);
		descriptor[0] = AN7552_RX_BUFFER_HOST + token * 2048;
		descriptor[2] = (AN7552_RX_TOKEN_BASE + token) << 16;
		descriptor[1] = (data[11] << 16) | (1U << 8);
		barrier();
		descriptor[3] = data[9] << 28;
		if (!next)
			data[9] = (data[9] + 1) & 15;
		barrier();
		*(volatile uint32_t *)(uintptr_t)(data[4] + 8) = producer;
		producer = next;
	}
	data[6] = producer;
}

static int native_ppe_submit(uint32_t token, uint32_t address, uint32_t length);
static volatile uint32_t *rx_metadata(uint32_t token)
{
	return rx_words(0x841b0000 + token * 32);
}
static volatile struct ppe_domain_saved *rx_domain(uint32_t token)
{
	return (void *)rx_words(AN7552_PPE_DOMAIN_HOST +
			       token * AN7552_PPE_DOMAIN_STRIDE);
}
static void native_rx_release(uint32_t token)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST),
			  *free_ids = rx_words(AN7552_RX_FREE_HOST);
	if (token >= AN7552_RX_BUFFER_COUNT || !states[token] ||
	    q[22] >= AN7552_RX_BUFFER_COUNT) {
		q[6] = 5;
		return;
	}
	rx_domain(token)->active = 0;
	states[token] = 0;
	free_ids[q[21]] = token;
	q[21] = (q[21] + 1) % AN7552_RX_BUFFER_COUNT;
	q[22]++;
}
static int native_rx_deliver(uint32_t token, uint32_t ppe_info)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *saved = rx_metadata(token), *data, *host,
			  *completion, *sidecar;
	volatile uint8_t *packet = (void *)rx_words(saved[0]), *destination;
	uint32_t band = (rx_words(AN7552_RX_STATE_HOST)[token] >> 8) & 1;
	uint32_t length = (saved[1] >> 16) & 16383, producer = q[2], host_index,
		 info, copied = 0;
	data = (void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RX_DATA_CONTROL +
				   band * 64);
	host_index = data[2];
	if (producer == (*rx_words(q[18]) & 65535) || host_index == data[5])
		return 0;
	host = rx_words(data[0] + host_index * 16);
	info = host[1];
	if ((info & (1U << 31)) || ((info >> 16) & 16383) < length ||
	    host[0] < 0x80000000 || host[0] > 0xa0000000 - length) {
		q[6] = 4;
		return 0;
	}
	ppe_domain_restore(packet, saved[6], rx_domain(token));
	destination = (void *)rx_words(host[0]);
	sidecar = rx_words(0x842f9000 + producer * 16);
	sidecar[0] = saved[4];
	sidecar[1] = saved[5];
	sidecar[2] = ppe_info;
	if (!(((uintptr_t)packet | (uintptr_t)destination) & 3))
		for (; copied + 4 <= length; copied += 4)
			*(volatile uint32_t *)(destination + copied) =
				*(volatile uint32_t *)(packet + copied);
	for (; copied < length; copied++)
		destination[copied] = packet[copied];
	completion = rx_words(q[0] + producer * 16);
	completion[0] = host[0];
	completion[1] = saved[1];
	completion[2] = (saved[2] & 65535) | (host[2] & 0xffff0000);
	barrier();
	host[1] = (info & 65535) | (length << 16) | (1U << 31) | (1U << 30);
	data[2] = (host_index + 1) % data[1];
	data[7]++;
	barrier();
	completion[3] = (saved[3] & 0x0fffffff) | (q[3] << 28);
	producer++;
	if (producer == q[1]) {
		producer = 0;
		q[3] = (q[3] + 1) & 15;
	}
	barrier();
	q[2] = producer;
	*(volatile uint32_t *)(uintptr_t)0x1ec0d1a8 = producer;
	native_rx_release(token);
	return 1;
}
static void native_rro_receive(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *states = rx_words(AN7552_RX_STATE_HOST), *descriptor,
			  *saved, *station;
	volatile uint8_t *packet;
	struct rx_metadata parsed = {0};
	uint32_t index, length, address, token, reason, cookie = 0, security,
							mode, band, split;
	if (!q[4]) {
		q[5] = 0;
		return;
	}
	q[5] = 1;
	if (q[6])
		return;
	native_rx_refill(0);
	native_rx_refill(1);
	index = q[16];
	descriptor = rx_words(AN7552_RX_COMPLETE_HOST + index * 16);
	if ((descriptor[3] >> 28) != q[17])
		return;
	barrier();
	length = (descriptor[1] >> 16) & 16383;
	address = descriptor[0];
	token = (descriptor[2] >> 16) - AN7552_RX_TOKEN_BASE;
	reason = (descriptor[2] >> 12) & 15;
	q[8] = length;
	q[9] = reason;
	q[15] = token;
	if (token >= AN7552_RX_BUFFER_COUNT || (states[token] & 255) != 1 ||
	    length > 2048 || address != AN7552_RX_BUFFER_HOST + token * 2048) {
		q[6] = 3;
		return;
	}
	packet = (void *)rx_words(address);
	saved = rx_metadata(token);
	band = (states[token] >> 8) & 1;
	if (!(states[token] & 0x10000)) {
		rx_domain(token)->active = 0;
		for (uint32_t i = 0; i < 4; i++)
			saved[i] = descriptor[i];
		/* Only a complete single-buffer frame can be parsed here. Later
		 * buffers carry arbitrary payload, not another RXD. Defer the
		 * whole split frame to the host's assembled-frame replay check.
		 */
		split = q[30] || !(descriptor[1] & (1U << 30));
		security = (split || reason == 1 || reason == 2) ?
				   0 :
				   native_rx_security(packet, length, &cookie);
		saved[4] = security;
		saved[5] = cookie;
		saved[6] = 0;
		saved[7] = 0;
		q[23] += ((security & 15) == 1);
		q[24] += ((security & 15) == 2);
		if (length >= 20)
			for (uint32_t i = 0; i < 5; i++)
				q[10 + i] = packet_word(packet + i * 4);
		mode = read32(AN7552_NPU_BASE + 0x6c0);
		uint32_t parsed_result =
			split ? RX_PARSE_FRAGMENT :
				rx_parse(packet, length, reason, &parsed);
		if ((security & 15) == 1) {
			q[25] = parsed_result;
			q[29] = packet_word(packet + 32);
			(*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE +
							   0x780 +
							   parsed_result *
								   4))++;
		}
		if ((mode == 2 || mode == AN7552_PPE_MODE_BRIDGE) &&
		    (security & 15) == 1 &&
		    parsed_result == RX_PARSE_OK &&
		    parsed.wcid == (security >> 4) && parsed.band < 2) {
			q[26]++;
			station = (void *)(uintptr_t)(AN7552_STATION_NPU +
						      parsed.wcid * 32);
			q[31] = station[1] ^ (parsed.band | (parsed.bss << 8));
			volatile uint8_t *source = packet + parsed.offset + 6;
			if (station_rx_authorized(station, parsed.band, source)) {
				saved[6] = parsed.offset;
				saved[7] = parsed.length;
			} else
				q[27]++;
		}
		states[token] |= 0x10000;
	}
	if (saved[7]) {
		uint32_t wcid = saved[4] >> 4;
		station = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
		if (!station[0] || !pn_entry(wcid)[0] ||
		    pn_entry(wcid)[1] != saved[5])
			saved[7] = 0;
	}
	if (saved[7]) {
		int ret = native_ppe_submit(token, address + saved[6], saved[7]);

		if (!ret)
			return;
		if (ret > 0)
			states[token] = 2 | (band << 8);
		else
			saved[7] = 0;
	}
	if (!saved[7] && !native_rx_deliver(token, 0))
		return;
	/* Advance fragment state only after the descriptor is consumed. */
	q[30] = !(descriptor[1] & (1U << 30));
	barrier();
	*rx_words(AN7552_RX_EMI_HOST) = index;
	index++;
	if (index == AN7552_RX_RING_COUNT) {
		index = 0;
		q[17] = (q[17] + 1) & 15;
	}
	q[16] = index;
	q[7]++;
}
