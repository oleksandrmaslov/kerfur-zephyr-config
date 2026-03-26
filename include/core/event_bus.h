#ifndef KERFUR_EVENT_BUS_H_
#define KERFUR_EVENT_BUS_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "core/app_event.h"

int app_event_bus_init(void);
int app_event_publish(enum app_event_type type, int32_t param);
int app_event_publish_with_timestamp(enum app_event_type type, int32_t param, int64_t timestamp_ms);
int app_event_publish_step_batch(int32_t steps, uint16_t hw_counter, uint8_t walking_confidence,
				 bool from_hw_counter);
int app_event_publish_step_batch_with_timestamp(int32_t steps, uint16_t hw_counter,
						uint8_t walking_confidence,
						bool from_hw_counter,
						int64_t timestamp_ms);
int app_event_publish_look_target(int16_t x, int16_t y, uint8_t confidence);
int app_event_publish_look_target_with_timestamp(int16_t x, int16_t y, uint8_t confidence,
						int64_t timestamp_ms);
int app_event_publish_carry_state(bool in_hand, uint8_t pickup_confidence,
				 uint8_t in_hand_confidence, uint8_t walking_confidence);
int app_event_publish_carry_state_with_timestamp(bool in_hand, uint8_t pickup_confidence,
						 uint8_t in_hand_confidence,
						 uint8_t walking_confidence,
						 int64_t timestamp_ms);
int app_event_publish_battery_percent(int8_t percent, bool known);
int app_event_publish_battery_percent_with_timestamp(int8_t percent, bool known,
						     int64_t timestamp_ms);
bool app_event_wait(struct app_event *event, k_timeout_t timeout);

#endif /* KERFUR_EVENT_BUS_H_ */
