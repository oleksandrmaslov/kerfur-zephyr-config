/* Host-test shim for <zephyr/logging/log.h>: compile log calls away. */
#ifndef KERFUR_NEARBY_HOST_SHIM_LOG_H_
#define KERFUR_NEARBY_HOST_SHIM_LOG_H_

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_ERR(...)
#define LOG_DBG(...)

#ifndef CONFIG_LOG_DEFAULT_LEVEL
#define CONFIG_LOG_DEFAULT_LEVEL 0
#endif

#endif /* KERFUR_NEARBY_HOST_SHIM_LOG_H_ */
