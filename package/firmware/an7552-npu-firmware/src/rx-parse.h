/* SPDX-License-Identifier: MIT */
#ifndef AN7552_RX_PARSE_H
#define AN7552_RX_PARSE_H
#include <stdint.h>
struct rx_metadata {
	uint32_t wcid, band, bss, key, tid, pn_low, pn_high;
	uint32_t offset, length, sequence;
};
enum rx_parse_result {
	RX_PARSE_OK,
	RX_PARSE_SHORT,
	RX_PARSE_TYPE,
	RX_PARSE_SECURITY,
	RX_PARSE_FRAGMENT,
	RX_PARSE_AGGREGATE,
	RX_PARSE_PROTOCOL
};
static uint32_t rx_get32(const volatile uint8_t *p)
{
	if (!((uintptr_t)p & 3))
		return *(const volatile uint32_t *)p;
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
/* Conservative fast-path classifier. All other frames retain the Linux path. */
static enum rx_parse_result rx_parse(const volatile uint8_t *packet,
				     uint32_t length, uint32_t reason,
				     struct rx_metadata *out)
{
	uint32_t d0, d1, d2, d3, offset, fc, sequence, qos, protocol;
	if (length < 32)
		return RX_PARSE_SHORT;
	d0 = rx_get32(packet);
	d1 = rx_get32(packet + 4);
	d2 = rx_get32(packet + 8);
	d3 = rx_get32(packet + 12);
	if ((d0 & 65535) != length)
		return RX_PARSE_SHORT;
	if ((d0 >> 27) != 2 || reason == 1 || reason == 2 || !(d2 & (1U << 7)))
		return RX_PARSE_TYPE;
	if (!(d1 & (1U << 31)) || (d1 & (15U << 23)) ||
	    (((d2 >> 16) & 31) != 4 && ((d2 >> 16) & 31) != 10 &&
	     ((d2 >> 16) & 31) != 11 && ((d2 >> 16) & 31) != 12) ||
	    (d2 & ((1U << 23) | (1U << 24) | (1U << 25))) ||
	    (d3 & (1U << 24)) || ((d3 >> 16) & 3) != 1)
		return RX_PARSE_SECURITY;
	if ((d1 & ((1U << 16) | (1U << 19))) != ((1U << 16) | (1U << 19)))
		return RX_PARSE_SECURITY;
	if (length < 64)
		return RX_PARSE_SHORT;
	fc = rx_get32(packet + 32) & 65535;
	sequence = rx_get32(packet + 40);
	qos = sequence >> 16;
	sequence &= 65535;
	if (((fc & 0xfc) != 0x08 && (fc & 0xfc) != 0x88) ||
	    (fc & 0x430c) != 0x4108 || (fc & 0x0400) || (sequence & 15) ||
	    (d2 & (1U << 27)))
		return RX_PARSE_FRAGMENT;
	offset = 64;
	if (d1 & (1U << 17))
		offset += 16;
	if (d1 & (1U << 18)) {
		offset += 16;
		if (d1 & (1U << 20))
			offset += 96;
	} else if (d1 & (1U << 20))
		return RX_PARSE_TYPE;
	offset += 2 * ((d2 >> 13) & 7);
	if (offset > length || length - offset < 14)
		return RX_PARSE_SHORT;
	protocol = ((uint32_t)packet[offset + 12] << 8) | packet[offset + 13];
	if ((packet[offset] & 1) || (protocol != 0x0800 && protocol != 0x86dd))
		return RX_PARSE_PROTOCOL;
	/* Extension headers and IP fragments retain the Linux path. */
	uint32_t ip = offset + 14, ip_size, l4, transport;
	if (protocol == 0x0800) {
		if (length - ip < 20 || (packet[ip] >> 4) != 4)
			return RX_PARSE_PROTOCOL;
		ip_size = (packet[ip] & 15) * 4;
		uint32_t total = ((uint32_t)packet[ip + 2] << 8) |
				 packet[ip + 3];
		if (ip_size < 20 || total < ip_size || total > length - ip ||
		    (((uint32_t)packet[ip + 6] << 8) | packet[ip + 7]) & 0x3fff)
			return RX_PARSE_PROTOCOL;
		transport = packet[ip + 9];
		l4 = total - ip_size;
	} else {
		if (length - ip < 40 || (packet[ip] >> 4) != 6)
			return RX_PARSE_PROTOCOL;
		ip_size = 40;
		l4 = ((uint32_t)packet[ip + 4] << 8) | packet[ip + 5];
		if (l4 > length - ip - 40)
			return RX_PARSE_PROTOCOL;
		transport = packet[ip + 6];
	}
	if (transport == 6) {
		if (l4 < 20 || (packet[ip + ip_size + 12] >> 4) < 5 ||
		    (packet[ip + ip_size + 12] >> 4) * 4 > l4)
			return RX_PARSE_PROTOCOL;
	} else if (transport == 17) {
		if (l4 < 8)
			return RX_PARSE_PROTOCOL;
	} else
		return RX_PARSE_PROTOCOL;
	out->wcid = d1 & 4095;
	out->band = (d1 >> 27) & 3;
	out->bss = d2 & 63;
	out->key = (d1 >> 21) & 3;
	out->tid = (fc & 0x80) ? (qos & 15) : 0;
	out->pn_low = rx_get32(packet + 48);
	out->pn_high = packet[52] | (uint32_t)packet[53] << 8;
	out->offset = offset;
	out->length = length - offset;
	out->sequence = sequence >> 4;
	return RX_PARSE_OK;
}
#endif
