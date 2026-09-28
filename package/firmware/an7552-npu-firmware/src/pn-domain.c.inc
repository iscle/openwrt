/* SPDX-License-Identifier: MIT */
/* Shared replay domain. Core 1 serializes every operation.
 * Per-link packet cookies and A-MSDU continuation state remain separate.
 */
#define PN_DOMAIN_COUNT AN7552_STATION_COUNT
#define PN_DOMAIN_INDEX_BITS AN7552_PN_DOMAIN_INDEX_BITS
#define PN_DOMAIN_GENERATION_MAX (UINT32_MAX >> PN_DOMAIN_INDEX_BITS)
#define PN_DOMAIN_CORE AN7552_PN_DOMAIN_NPU

static volatile uint32_t *pn_domain_slot(uint32_t index)
{
	return (void *)(uintptr_t)(PN_DOMAIN_CORE + index * AN7552_PN_ENTRY_SIZE);
}

static volatile uint32_t *pn_domain_lookup(uint32_t handle, uint32_t key)
{
	uint32_t generation = handle >> PN_DOMAIN_INDEX_BITS;
	volatile uint32_t *domain;

	if (!generation || key > 3)
		return 0;
	domain = pn_domain_slot(handle & (PN_DOMAIN_COUNT - 1));
	if (!domain[0] || domain[1] != generation || domain[2] != key)
		return 0;
	return domain;
}

static void pn_domain_store(volatile uint32_t *counter,
			    uint32_t low, uint32_t high)
{
	/* A fault between stores may advance the limit conservatively, but
	 * cannot decrease it when the low word wraps.
	 */
	counter[1] = high;
	barrier();
	counter[0] = low;
	barrier();
}

static uint32_t pn_domain_initial_valid(const uint32_t *initial)
{
	if (!initial)
		return 0;
	for (uint32_t tid = 0; tid < 17; tid++)
		if (initial[tid * 2 + 1] > 65535)
			return 0;
	return 1;
}

static void pn_domain_merge(volatile uint32_t *domain, const uint32_t *initial)
{
	for (uint32_t tid = 0; tid < 17; tid++) {
		volatile uint32_t *counter = domain + 4 + tid * 2;
		uint32_t low = initial[tid * 2];
		uint32_t high = initial[tid * 2 + 1];

		if (high > counter[1] || (high == counter[1] && low > counter[0]))
			pn_domain_store(counter, low, high);
	}
}

/* Every returned handle owns one reference. Zero means allocation failure. */
static uint32_t pn_domain_create(uint32_t key, const uint32_t *initial)
{
	if (key > 3 || !pn_domain_initial_valid(initial))
		return 0;
	for (uint32_t index = 0; index < PN_DOMAIN_COUNT; index++) {
		volatile uint32_t *domain = pn_domain_slot(index);
		uint32_t generation = domain[1];

		/* Retire exhausted generations rather than accepting a stale handle. */
		if (domain[0] || generation >= PN_DOMAIN_GENERATION_MAX)
			continue;
		domain[1] = ++generation;
		domain[2] = key;
		for (uint32_t i = 0; i < 34; i++)
			domain[4 + i] = initial[i];
		barrier();
		domain[0] = 1;
		barrier();
		return (generation << PN_DOMAIN_INDEX_BITS) | index;
	}
	return 0;
}

static uint32_t pn_domain_attach(uint32_t handle, uint32_t key,
				const uint32_t *initial)
{
	volatile uint32_t *domain = pn_domain_lookup(handle, key);

	if (!domain || domain[0] >= PN_DOMAIN_COUNT ||
	    !pn_domain_initial_valid(initial))
		return 0;
	pn_domain_merge(domain, initial);
	domain[0]++;
	barrier();
	return 1;
}

static uint32_t pn_domain_release(uint32_t handle, uint32_t key)
{
	volatile uint32_t *domain = pn_domain_lookup(handle, key);

	if (!domain)
		return 0;
	domain[0]--;
	barrier();
	return 1;
}

static uint32_t pn_domain_compare(uint32_t handle, uint32_t key, uint32_t tid,
				 uint32_t low, uint32_t high)
{
	volatile uint32_t *domain = pn_domain_lookup(handle, key);
	volatile uint32_t *counter;

	if (!domain || tid > 16 || high > 65535)
		return 3;
	counter = domain + 4 + tid * 2;
	if (high < counter[1] || (high == counter[1] && low <= counter[0]))
		return 0;
	pn_domain_store(counter, low, high);
	return 1;
}

static uint32_t pn_domain_snapshot(uint32_t handle, uint32_t key, uint32_t *pn)
{
	volatile uint32_t *domain = pn_domain_lookup(handle, key);

	if (!domain || !pn)
		return 0;
	for (uint32_t i = 0; i < 34; i++)
		pn[i] = domain[4 + i];
	return 1;
}
