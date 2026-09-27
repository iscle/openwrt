/* SPDX-License-Identifier: MIT */
static int native_ppe_submit(uint32_t token, uint32_t address, uint32_t length)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	uint32_t producer = q[10], next = (producer + 1) % 64;
	volatile uint32_t *descriptor = rx_words(0x842f0000 + producer * 32);
	if (q[0] != 2 || q[3] || next == read32(0x1fb5080c) ||
	    !(descriptor[1] & 0x80000000))
		return 0;
	descriptor[2] = address;
	descriptor[4] = 0x80000000 | (token << 14);
	descriptor[5] = 0x7f4087ff;
	barrier();
	descriptor[1] = length;
	barrier();
	*(volatile uint32_t *)(uintptr_t)0x1fb50808 = next;
	q[10] = next;
	q[1]++;
	q[12]++;
	return 1;
}
static void native_ppe_receive(void)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	uint32_t status, metadata, token;
	for (uint32_t budget = 0; budget < 32; budget++) {
		if (!(read32(0x1fb50fe4) & 65535))
			return;
		status = read32(0x1fb50fe0);
		if (!(status & 0x80000000))
			return;
		metadata = read32(0x1fb50fdc);
		token = status & 65535;
		q[7] = status;
		q[8] = metadata;
		if (token >= AN7552_RX_BUFFER_COUNT ||
		    (rx_words(AN7552_RX_STATE_HOST)[token] & 255) != 2 ||
		    !q[12]) {
			q[3] = 4;
			return;
		}
		if (q[0] == 3 || (status & 0x40000000)) {
			native_rx_release(token);
			q[14]++;
		} else {
			if (!native_rx_deliver(token, (metadata & 0x1fffff) |
							      (1U << 29)))
				return;
			q[15]++;
		}
		*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0x80000000;
		q[2]++;
		q[9]++;
		q[12]--;
	}
}
static void native_ppe_service(void)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	volatile uint32_t *descriptor;
	uint32_t producer, status;
	if (!q[0]) {
		q[13] = 0;
		return;
	}
	q[13] = 1;
	if (q[0] >= 2) {
		if (!q[3])
			native_ppe_receive();
		return;
	}
	if (q[3] || q[1] == q[2])
		return;
	if (q[12]) {
		if (!(read32(0x1fb50fe4) & 65535))
			return;
		status = read32(0x1fb50fe0);
		if (!(status & 0x80000000))
			return;
		q[7] = status;
		q[8] = read32(0x1fb50fdc);
		q[9]++;
		if ((status & 65535) != q[6])
			q[3] = 3;
		*(volatile uint32_t *)(uintptr_t)0x1fb50fe0 = 0x80000000;
		barrier();
		q[12] = 0;
		q[2] = q[1];
		return;
	}
	if (q[4] != 0x842f8000 || q[5] < 14 || q[5] > 2048 || q[6] > 65535) {
		q[3] = 1;
		return;
	}
	producer = q[10];
	if (producer >= 64) {
		q[3] = 2;
		return;
	}
	q[11] = read32(0x1fb5080c);
	descriptor = (void *)(uintptr_t)(0x442f0000 + producer * 32);
	if (!(descriptor[1] & 0x80000000))
		return;
	descriptor[2] = q[4];
	descriptor[4] = 0x80000000 | (q[6] << 14);
	descriptor[5] = 0x7f4087ff;
	barrier();
	descriptor[1] = q[5];
	barrier();
	producer = (producer + 1) % 64;
	*(volatile uint32_t *)(uintptr_t)0x1fb50808 = producer;
	q[10] = producer;
	q[12] = 1;
}
