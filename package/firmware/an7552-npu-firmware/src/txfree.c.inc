/* SPDX-License-Identifier: MIT */
/* Completion parsing runs before publishing the buffer to Linux. */
static uint32_t packet_word(volatile uint8_t *p)
{
	if (!((uintptr_t)p & 3))
		return *(const volatile uint32_t *)p;
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int txfree_walk(volatile uint8_t *packet, uint32_t length,
		       volatile uint32_t *control, int commit)
{
	uint32_t total, version, count = 0, offset = 8;

	if (length < 8)
		return 1;
	if ((packet_word(packet) >> 27) != 6)
		return 0;
	total = (packet_word(packet) >> 16) & 1023;
	version = (packet_word(packet + 4) >> 16) & 15;
	if (version < 5 || version > 7)
		return 2;
	while (count < total) {
		uint32_t info;

		if (offset + 4 > length)
			return 3;
		info = packet_word(packet + offset);
		offset += 4;
		if (info & (1U << 31)) {
			if (version == 7 && offset + 4 <= length &&
			    (packet_word(packet + offset) & (1U << 31)))
				offset += 4;
			continue;
		}
		if (info & (1U << 30))
			continue;
		for (uint32_t i = 0; i < 2; i++) {
			uint32_t token = (info >> (15 * i)) & 32767;
			volatile uint32_t *work;

			if (token == 32767)
				continue;
			if (++count > total || token >= AN7552_TXWI_TOKENS)
				return 4;
			if (!commit)
				continue;
			if (token >= AN7552_DIRECT_TOKENS) {
				control[9]++;
				continue;
			}
			work = (void *)(uintptr_t)(AN7552_FORWARD_WORK_NPU +
						   token * 16);
			if (work[0] != 2)
				return 5;
			barrier();
			work[0] = 3;
			control[8]++;
		}
	}
	return 0;
}

static void native_txfree(uint32_t band)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_TXFREE_CONTROL +
				    band * 64);
	volatile uint32_t *descriptor, *host;
	volatile uint8_t *packet, *destination;
	uint32_t index, producer, info, length, result, address, capacity;
	uint32_t hardware = AN7552_TXFREE_RING_HOST + band * 0x2000;

	if (!q[4]) {
		q[5] = 0;
		return;
	}
	q[5] = 1;
	if (q[6])
		return;
	index = q[12];
	producer = q[2];
	if (index == read32(q[3] + 12) || producer == q[15])
		return;
	descriptor =
		(void *)(uintptr_t)(((hardware & 0x3fffffff) | 0x40000000) +
				    index * 16);
	info = descriptor[1];
	if (!(info & (1U << 31)))
		return;
	barrier();
	length = (info >> 16) & 16383;
	address = descriptor[0];
	if (!(info & (1U << 30)) || !length || length > 2048 ||
	    address != AN7552_TXFREE_BUFFER_HOST + band * 0x100000 +
			       index * 2048) {
		q[6] = 6;
		return;
	}
	host = (void *)(uintptr_t)(((q[0] & 0x3fffffff) | 0x40000000) +
				   producer * 16);
	capacity = (host[1] >> 16) & 16383;
	if ((host[1] & (1U << 31)) || capacity < length ||
	    host[0] < 0x80000000 || host[0] > 0xa0000000 - length) {
		q[6] = 7;
		return;
	}
	packet = (void *)(uintptr_t)((address & 0x3fffffff) | 0x40000000);
	result = txfree_walk(packet, length, q, 0);
	if (!result)
		result = txfree_walk(packet, length, q, 1);
	if (result) {
		q[6] = result;
		return;
	}
	/* Preserve host completions and station statistics for the normal driver. */
	destination = (void *)(uintptr_t)((host[0] & 0x3fffffff) | 0x40000000);
	for (uint32_t i = 0; i < length; i++)
		destination[i] = packet[i];
	host[3] = descriptor[3];
	barrier();
	host[1] = info;
	q[7]++;
	barrier();
	q[2] = (producer + 1) % q[1];
	/* Updating the virtual RX producer raises the NPU host interrupt. */
	*(volatile uint32_t *)(uintptr_t)(0x1ec0d188 + band * 16) = q[2];
	descriptor[1] = 2048U << 16;
	barrier();
	*(volatile uint32_t *)(uintptr_t)(q[3] + 8) = index;
	q[12] = (index + 1) % AN7552_TXFREE_COUNT;
}
