/* SPDX-License-Identifier: MIT */
#define PN_TABLE_HOST 0x84110000U
#define PN_STAGING_HOST 0x841a0000U
#define PN_CONTROL 0x700U
#include "pn-domain.c.inc"

static volatile uint32_t *pn_entry(uint32_t wcid)
{
	return (void *)(uintptr_t)(0x44110000 + wcid * 256);
}

/* A raw MLO reorder WCID can carry packets from both radios. */
static volatile uint32_t *pn_link_state(uint32_t wcid, uint32_t band)
{
	if (!band)
		return pn_entry(wcid);
	return (void *)(uintptr_t)(AN7552_PN_RADIO_STATE_NPU +
				   wcid * AN7552_PN_ENTRY_SIZE);
}

static uint32_t pn_compare(uint32_t wcid, uint32_t tid, uint32_t low,
			   uint32_t high)
{
	volatile uint32_t *entry;

	if (wcid >= AN7552_STATION_COUNT || tid > 16 || high > 65535)
		return 3;
	entry = pn_entry(wcid);
	if (!entry[0])
		return 2;
	return pn_domain_compare(entry[2], entry[3], tid, low, high);
}

static void pn_detach(volatile uint32_t *entry)
{
	if (entry[2])
		pn_domain_release(entry[2], entry[3]);
	entry[2] = 0;
}

static uint32_t pn_install(uint32_t wcid, uint32_t enable, uint32_t handle,
			   const uint32_t *initial)
{
	volatile uint32_t *entry = pn_entry(wcid);
	uint32_t key = enable >> 8;

	entry[0] = 0;
	entry[60] = 0;
	barrier();
	if (enable && ((enable & 255) != 1 && (enable & 255) != 3))
		return 0;
	if (key > 3)
		return 0;
	/* Retire an exhausted cookie until a complete device restart. */
	if (entry[1] == UINT32_MAX) {
		pn_detach(entry);
		return 0;
	}
	if (enable && handle) {
		/* Acquire before dropping the last reference to this same domain. */
		if (!pn_domain_attach(handle, key, initial))
			return 0;
		pn_detach(entry);
	} else {
		pn_detach(entry);
		if (enable) {
			handle = pn_domain_create(key, initial);
			if (!handle)
				return 0;
		}
	}
	entry[2] = enable ? handle : 0;
	entry[3] = key;
	for (uint32_t band = 0; band < 2; band++) {
		volatile uint32_t *local = pn_link_state(wcid, band);

		for (uint32_t i = 0; i < 34; i++)
			local[4 + i] = 0;
		for (uint32_t tid = 0; tid < 17; tid++)
			local[40 + tid] = 0;
	}
	entry[1]++;
	barrier();
	if (enable & 2)
		entry[60] = 1 | (key << 8);
	else
		entry[0] = enable;
	return 1;
}

/* All per-link snapshots must be merged and radio keys installed before
 * any link can accept RX with the new common counter. Core 1 processes the
 * complete commit without interleaving receive work between link updates.
 */
static uint32_t pn_commit(uint32_t count, uint32_t key, const uint32_t *wcids)
{
	uint32_t handle = 0;

	if (!count || count > 16 || key > 3)
		return 0;
	for (uint32_t i = 0; i < count; i++) {
		volatile uint32_t *entry;

		if (wcids[i] >= AN7552_STATION_COUNT)
			return 0;
		for (uint32_t j = 0; j < i; j++)
			if (wcids[j] == wcids[i])
				return 0;
		entry = pn_entry(wcids[i]);
		if (entry[0] || entry[60] != (1 | (key << 8)) ||
		    !pn_domain_lookup(entry[2], key))
			return 0;
		if (handle && handle != entry[2])
			return 0;
		handle = entry[2];
	}
	for (uint32_t i = 0; i < count; i++) {
		volatile uint32_t *entry = pn_entry(wcids[i]);

		entry[0] = entry[60];
		entry[60] = 0;
	}
	barrier();
	return 1;
}

static void native_pn_service(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
	volatile uint32_t *staging = (void *)(uintptr_t)0x441a0000;
	volatile uint32_t *entry;
	uint32_t request = q[0], result = 3, initial[34];

	if (!request || request == q[8] || q[9])
		return;
	barrier();
	q[14] = 0;
	if (q[1] == 1) {
		result = pn_compare(q[2], q[3], q[4], q[5]);
	} else if (q[1] == 2 && q[2] < AN7552_STATION_COUNT) {
		for (uint32_t i = 0; i < 34; i++)
			initial[i] = staging[i];
		if (pn_install(q[2], q[6], q[3], initial)) {
			q[14] = pn_entry(q[2])[2];
			result = 1;
		}
	} else if (q[1] == 3 && q[2] < AN7552_STATION_COUNT) {
		entry = pn_entry(q[2]);
		if (q[6] <= 3 && entry[0] == (1 | (q[6] << 8)) &&
		    pn_domain_snapshot(entry[2], q[6], initial)) {
			for (uint32_t i = 0; i < 34; i++)
				staging[i] = initial[i];
			entry[0] = 0;
			q[14] = entry[2];
			result = 1;
		} else {
			result = 2;
		}
	} else if (q[1] == 4 && q[2] <= 16) {
		for (uint32_t i = 0; i < q[2]; i++)
			initial[i] = staging[i];
		if (pn_commit(q[2], q[6], initial))
			result = 1;
	}
	q[10]++;
	q[11] += result == 1;
	q[12] += result == 0;
	q[13] += result > 1;
	q[7] = result;
	barrier();
	q[8] = request;
}
