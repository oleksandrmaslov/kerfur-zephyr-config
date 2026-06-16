/* Host-test shim for <zephyr/sys/byteorder.h>. The byte-buffer helpers
 * (put/get_le32) are endianness-explicit and portable; the integer
 * conversions assume a little-endian host (true for x86/arm64 dev machines). */
#ifndef KERFUR_NEARBY_HOST_SHIM_BYTEORDER_H_
#define KERFUR_NEARBY_HOST_SHIM_BYTEORDER_H_

#include <stdint.h>

static inline uint16_t sys_cpu_to_le16(uint16_t v) { return v; }
static inline uint16_t sys_le16_to_cpu(uint16_t v) { return v; }
static inline uint32_t sys_cpu_to_le32(uint32_t v) { return v; }
static inline uint32_t sys_le32_to_cpu(uint32_t v) { return v; }
static inline uint64_t sys_cpu_to_le64(uint64_t v) { return v; }
static inline uint64_t sys_le64_to_cpu(uint64_t v) { return v; }

static inline void sys_put_le32(uint32_t val, uint8_t dst[4])
{
	dst[0] = (uint8_t)(val & 0xFF);
	dst[1] = (uint8_t)((val >> 8) & 0xFF);
	dst[2] = (uint8_t)((val >> 16) & 0xFF);
	dst[3] = (uint8_t)((val >> 24) & 0xFF);
}

static inline uint32_t sys_get_le32(const uint8_t src[4])
{
	return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
	       ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

#endif /* KERFUR_NEARBY_HOST_SHIM_BYTEORDER_H_ */
