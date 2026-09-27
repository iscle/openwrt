/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ppe-domain.h"

int main(void)
{
	uint8_t buffer[2048], original[2048];
	struct ppe_domain_saved saved = { 0 };
	unsigned int checks = 0;

	for (unsigned int offset = 4; offset < 200; offset++) {
		for (unsigned int domain = 1; domain <= 2048; domain++) {
			for (unsigned int i = 0; i < sizeof(buffer); i++)
				buffer[i] = i * 73 + offset;
			buffer[offset] &= ~1;
			buffer[offset + 12] = domain & 1 ? 0x08 : 0x86;
			buffer[offset + 13] = domain & 1 ? 0x00 : 0xdd;
			memcpy(original, buffer, sizeof(buffer));
			assert(ppe_domain_push(buffer, sizeof(buffer), offset,
					       sizeof(buffer) - offset, domain,
					       &saved));
			assert(!memcmp(buffer + offset - 4, original + offset,
				       12));
			assert(buffer[offset + 8] == 0x81 &&
			       !buffer[offset + 9]);
			assert(buffer[offset + 10] == domain >> 8);
			assert(buffer[offset + 11] == (uint8_t)domain);
			assert(!memcmp(buffer, original, offset - 4));
			assert(!memcmp(buffer + offset + 12,
				       original + offset + 12,
				       sizeof(buffer) - offset - 12));
			assert(!ppe_domain_push(buffer, sizeof(buffer), offset,
						sizeof(buffer) - offset, domain,
						&saved));
			ppe_domain_restore(buffer, offset, &saved);
			assert(!memcmp(buffer, original, sizeof(buffer)));
			ppe_domain_restore(buffer, offset, &saved);
			assert(!memcmp(buffer, original, sizeof(buffer)));
			checks++;
		}
	}
	for (unsigned int offset = 0; offset < sizeof(buffer) + 4; offset++) {
		assert(!ppe_domain_push(buffer, sizeof(buffer), offset,
					UINT32_MAX, 1, &saved));
		assert(!memcmp(buffer, original, sizeof(buffer)));
	}
	for (unsigned int domain = 2049; domain <= 65535; domain++)
		assert(!ppe_domain_push(buffer, sizeof(buffer), 100, 100,
					domain, &saved));
	assert(!ppe_domain_push(buffer, sizeof(buffer), 100, 100, 0, &saved));
	assert(!ppe_domain_push(buffer, sizeof(buffer), 3, 100, 1, &saved));
	for (unsigned int length = 0; length < 14; length++)
		assert(!ppe_domain_push(buffer, sizeof(buffer), 100, length, 1,
					&saved));
	for (unsigned int proto = 0; proto <= 65535; proto++) {
		if (proto == 0x0800 || proto == 0x86dd)
			continue;
		buffer[112] = proto >> 8;
		buffer[113] = proto;
		assert(!ppe_domain_push(buffer, sizeof(buffer), 100, 100, 1,
					&saved));
	}
	buffer[100] |= 1;
	buffer[112] = 8;
	buffer[113] = 0;
	assert(!ppe_domain_push(buffer, sizeof(buffer), 100, 100, 1, &saved));
	printf("PASS: %u bridge-domain transforms, exact restoration and invalid-input rejection\n",
	       checks);
	return 0;
}
