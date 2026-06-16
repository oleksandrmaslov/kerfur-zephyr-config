/* Host-test shim for <zephyr/kernel.h>: the few kernel primitives the nearby
 * module + encounter log use. Single-threaded host, so mutexes are no-ops and
 * the "uptime" clock is a controllable global (kerfur_host_now_ms) — the state
 * machine itself is driven by explicit now_ms args, so this only affects the
 * friend-resolution helpers that call k_uptime_get() internally. */
#ifndef KERFUR_NEARBY_HOST_SHIM_KERNEL_H_
#define KERFUR_NEARBY_HOST_SHIM_KERNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t (used by settings_read_cb / the module) */

#include <zephyr/sys/util.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif

typedef struct { long ticks; } k_timeout_t;
#define K_FOREVER ((k_timeout_t){ -1 })
#define K_NO_WAIT ((k_timeout_t){ 0 })
#define K_MSEC(ms) ((k_timeout_t){ (ms) })

struct k_mutex { int _held; };
/* Used as `static K_MUTEX_DEFINE(name);` in the module, so the macro must NOT
 * add its own storage-class specifier. */
#define K_MUTEX_DEFINE(name) struct k_mutex name = { 0 }

static inline int k_mutex_init(struct k_mutex *m) { (void)m; return 0; }
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t)
{
	(void)m; (void)t; return 0;
}
static inline int k_mutex_unlock(struct k_mutex *m) { (void)m; return 0; }

/* Controllable monotonic clock for the host. The test owns it. */
extern int64_t kerfur_host_now_ms;
static inline int64_t k_uptime_get(void) { return kerfur_host_now_ms; }
static inline uint32_t k_uptime_get_32(void) { return (uint32_t)kerfur_host_now_ms; }

#endif /* KERFUR_NEARBY_HOST_SHIM_KERNEL_H_ */
