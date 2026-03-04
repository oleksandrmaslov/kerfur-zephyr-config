#ifndef KERFUR_APP_EVENT_H_
#define KERFUR_APP_EVENT_H_

#include <stdint.h>

enum app_event_type {
	APP_EVENT_TICK_1S = 0,
	APP_EVENT_USER_BUTTON_PRESS,
	APP_EVENT_MOCK_PET,
	APP_EVENT_MOCK_SHAKE,
	APP_EVENT_MOCK_NOTIFICATION,
	APP_EVENT_PHONE_NOTIFICATION,
	APP_EVENT_IDLE_TIMEOUT,
	APP_EVENT_WAKE,
	APP_EVENT_SLEEP_REQUEST,
	APP_EVENT_BATTERY_LOW,
	APP_EVENT_BLE_CONNECTED,
	APP_EVENT_BLE_DISCONNECTED,
	APP_EVENT_COUNT
};

struct app_event {
	enum app_event_type type;
	int64_t timestamp_ms;
	int32_t param;
};

const char *app_event_type_str(enum app_event_type type);

#endif /* KERFUR_APP_EVENT_H_ */
