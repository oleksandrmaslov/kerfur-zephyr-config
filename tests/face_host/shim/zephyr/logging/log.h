/* Host-test shim for <zephyr/logging/log.h>.
 *
 * The face runtime only uses logging for debug dumps; on the host we
 * compile those calls away to nothing. This lets the real face_runtime.c
 * build and run on a desktop without the Zephyr logging subsystem.
 */
#ifndef KERFUR_FACE_HOST_SHIM_LOG_H_
#define KERFUR_FACE_HOST_SHIM_LOG_H_

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_ERR(...)
#define LOG_DBG(...)

#ifndef CONFIG_LOG_DEFAULT_LEVEL
#define CONFIG_LOG_DEFAULT_LEVEL 0
#endif

#endif /* KERFUR_FACE_HOST_SHIM_LOG_H_ */
