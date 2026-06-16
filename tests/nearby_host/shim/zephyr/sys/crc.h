/* Host-test shim for <zephyr/sys/crc.h>: standard CRC-32/IEEE (reflected,
 * poly 0xEDB88420, init/final 0xFFFFFFFF) — matches Zephyr's crc32_ieee() so
 * friend ephemeral-ID resolution behaves identically off-target. */
#ifndef KERFUR_NEARBY_HOST_SHIM_CRC_H_
#define KERFUR_NEARBY_HOST_SHIM_CRC_H_

#include <stddef.h>
#include <stdint.h>

static inline uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++) {
			uint32_t mask = ~((crc & 1U) - 1U);

			crc = (crc >> 1) ^ (0xEDB88420U & mask);
		}
	}
	return crc ^ 0xFFFFFFFFU;
}

#endif /* KERFUR_NEARBY_HOST_SHIM_CRC_H_ */
