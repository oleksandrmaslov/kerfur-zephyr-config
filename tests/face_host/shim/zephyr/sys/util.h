/* Host-test shim for <zephyr/sys/util.h>: just the few helpers the face
 * runtime and generated tables use. Mirrors Zephyr semantics closely
 * enough for off-target unit testing. */
#ifndef KERFUR_FACE_HOST_SHIM_UTIL_H_
#define KERFUR_FACE_HOST_SHIM_UTIL_H_

#include <stddef.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(v, lo, hi) (((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) ((void)(x))
#endif
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef MSEC_PER_SEC
#define MSEC_PER_SEC 1000
#endif

/* Zephyr IS_ENABLED(): evaluates to 1 only when the config token is
 * defined to 1, and to 0 otherwise (including undefined). */
#define IS_ENABLED(config_macro) _IS_ENABLED1(config_macro)
#define _IS_ENABLED1(config_macro) _IS_ENABLED2(_XXXX##config_macro)
#define _XXXX1 _YYYY,
#define _IS_ENABLED2(one_or_two_args) _IS_ENABLED3(one_or_two_args 1, 0)
#define _IS_ENABLED3(ignore_this, val, ...) val

#ifndef BUILD_ASSERT
#define BUILD_ASSERT(EXPR, ...) _Static_assert(EXPR, "" __VA_ARGS__)
#endif

#endif /* KERFUR_FACE_HOST_SHIM_UTIL_H_ */
