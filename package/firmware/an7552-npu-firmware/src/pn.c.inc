/* SPDX-License-Identifier: MIT */
#define PN_TABLE_HOST 0x84110000U
#define PN_STAGING_HOST 0x841a0000U
#define PN_CONTROL 0x700U
static volatile uint32_t *pn_entry(uint32_t wcid)
{
	return (void *)(uintptr_t)(0x44110000 + wcid * 256);
}
/* Core 1 alone updates counters; both hardware and Linux use this operation. */
static uint32_t pn_compare(uint32_t wcid, uint32_t tid, uint32_t low,
			   uint32_t high)
{
	volatile uint32_t *entry;
	uint32_t old_low, old_high;
	if (wcid >= AN7552_STATION_COUNT || tid > 16 || high > 65535)
		return 3;
	entry = pn_entry(wcid);
	if (!entry[0])
		return 2;
	old_low = entry[4 + tid * 2];
	old_high = entry[5 + tid * 2];
	if (high < old_high || (high == old_high && low <= old_low))
		return 0;
	entry[4 + tid * 2] = low;
	entry[5 + tid * 2] = high;
	entry[40 + tid] = 0;
	return 1;
}
static void native_pn_service(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
	volatile uint32_t *entry, *initial = (void *)(uintptr_t)0x441a0000;
	uint32_t request = q[0], result = 3;
	if (!request || request == q[8] || q[9])
		return;
	barrier();
	if (q[1] == 1)
		result = pn_compare(q[2], q[3], q[4], q[5]);
	else if (q[1] == 2 && q[2] < AN7552_STATION_COUNT) {
		entry = pn_entry(q[2]);
		entry[0] = 0;
		for (uint32_t i = 0; i < 34; i++)
			entry[4 + i] = initial[i];
		for (uint32_t i = 0; i < 17; i++)
			entry[40 + i] = 0;
		entry[1]++;
		barrier();
		entry[0] = q[6];
		result = 1;
	} else if (q[1] == 3 && q[2] < AN7552_STATION_COUNT) {
		entry = pn_entry(q[2]);
		if (entry[0] == (1 | (q[6] << 8))) {
			for (uint32_t i = 0; i < 34; i++)
				initial[i] = entry[4 + i];
			/* Keep the snapshot current until the host reinstalls the key. */
			entry[0] = 0;
			result = 1;
		} else {
			result = 2;
		}
	}
	q[10]++;
	q[11] += (result == 1);
	q[12] += (result == 0);
	q[13] += (result > 1);
	q[7] = result;
	barrier();
	q[8] = request;
}
