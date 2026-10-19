/* SPDX-License-Identifier: MIT */
/* Single-frame submission through the batched queue; used by host tests. */
__attribute__((unused)) static int
native_ppe_submit(uint32_t token, uint32_t address, uint32_t length)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	struct rro_batch b = { 0 };
	int ret;

	b.mode = q[0];
	b.ppe_producer = q[10];
	b.ppe_hw = read32(0x1fb5080c);
	ret = native_ppe_queue(&b, token, address, length);
	if (ret <= 0)
		return ret;
	native_ppe_flush(&b);
	return 1;
}
static void native_ppe_receive(void)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	uint32_t status, metadata, token;
	for (uint32_t budget = 0; budget < 64; budget++) {
		if (!(read32(0x1fb50fe4) & 65535))
			return;
		status = read32(0x1fb50fe0);
		if (!(status & 0x80000000))
			return;
		metadata = read32(0x1fb50fdc);
		token = status & 65535;
		if (token >= AN7552_RX_BUFFER_COUNT ||
		    (st_get(token) & 255) != 2 ||
		    !q[12]) {
			q[3] = 4;
			return;
		}
		if (q[0] == AN7552_PPE_MODE_DRAIN || (status & 0x40000000)) {
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
		q[12]--;
	}
}
static void native_ppe_service(void)
{
	volatile uint32_t *q = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);

	if (!q[0]) {
		q[13] = 0;
		return;
	}
	q[13] = 1;
	if (q[0] != AN7552_PPE_MODE_DRAIN && q[0] != AN7552_PPE_MODE_BRIDGE) {
		q[3] = 1;
		return;
	}
	if (!q[3])
		native_ppe_receive();
}
