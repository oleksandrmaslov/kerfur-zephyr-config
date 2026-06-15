/* Host-test shim for <zephyr/random/random.h>: back sys_rand32_get() with
 * the C library RNG. Seed with srand() in the test for reproducibility. */
#ifndef KERFUR_FACE_HOST_SHIM_RANDOM_H_
#define KERFUR_FACE_HOST_SHIM_RANDOM_H_

#include <stdint.h>
#include <stdlib.h>

static inline uint32_t sys_rand32_get(void)
{
	return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

#endif /* KERFUR_FACE_HOST_SHIM_RANDOM_H_ */
