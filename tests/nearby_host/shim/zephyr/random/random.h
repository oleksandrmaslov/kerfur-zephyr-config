/* Host-test shim for <zephyr/random/random.h>. Backs the RNG with the C
 * library; seed with srand() in the test for reproducibility. The nearby
 * module only uses sys_rand_get() (to mint the device secret at init). */
#ifndef KERFUR_NEARBY_HOST_SHIM_RANDOM_H_
#define KERFUR_NEARBY_HOST_SHIM_RANDOM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static inline uint32_t sys_rand32_get(void)
{
	return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static inline void sys_rand_get(void *dst, size_t len)
{
	uint8_t *p = (uint8_t *)dst;

	for (size_t i = 0; i < len; i++) {
		p[i] = (uint8_t)(rand() & 0xFF);
	}
}

#endif /* KERFUR_NEARBY_HOST_SHIM_RANDOM_H_ */
