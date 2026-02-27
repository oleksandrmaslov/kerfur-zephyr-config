#ifndef KERFUR_EVENT_BUS_H_
#define KERFUR_EVENT_BUS_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "core/app_event.h"

int app_event_bus_init(void);
int app_event_publish(enum app_event_type type, int32_t param);
int app_event_publish_with_timestamp(enum app_event_type type, int32_t param, int64_t timestamp_ms);
bool app_event_wait(struct app_event *event, k_timeout_t timeout);

#endif /* KERFUR_EVENT_BUS_H_ */
