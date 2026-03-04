#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/app_event.h"
#include "core/event_bus.h"

LOG_MODULE_REGISTER(event_bus, CONFIG_LOG_DEFAULT_LEVEL);

#define APP_EVENT_QUEUE_LEN 32

K_MSGQ_DEFINE(g_event_queue, sizeof(struct app_event), APP_EVENT_QUEUE_LEN, 4);

static const char *const g_event_names[APP_EVENT_COUNT] = {
	[APP_EVENT_TICK_1S] = "TICK_1S",
	[APP_EVENT_USER_BUTTON_PRESS] = "USER_BUTTON_PRESS",
	[APP_EVENT_MOCK_PET] = "MOCK_PET",
	[APP_EVENT_MOCK_SHAKE] = "MOCK_SHAKE",
	[APP_EVENT_MOCK_NOTIFICATION] = "MOCK_NOTIFICATION",
	[APP_EVENT_PHONE_NOTIFICATION] = "PHONE_NOTIFICATION",
	[APP_EVENT_IDLE_TIMEOUT] = "IDLE_TIMEOUT",
	[APP_EVENT_WAKE] = "WAKE",
	[APP_EVENT_SLEEP_REQUEST] = "SLEEP_REQUEST",
	[APP_EVENT_BATTERY_LOW] = "BATTERY_LOW",
	[APP_EVENT_BLE_CONNECTED] = "BLE_CONNECTED",
	[APP_EVENT_BLE_DISCONNECTED] = "BLE_DISCONNECTED",
};

int app_event_bus_init(void)
{
	k_msgq_purge(&g_event_queue);
	LOG_INF("Event bus initialized (queue=%d)", APP_EVENT_QUEUE_LEN);
	return 0;
}

int app_event_publish_with_timestamp(enum app_event_type type, int32_t param, int64_t timestamp_ms)
{
	struct app_event event = {
		.type = type,
		.timestamp_ms = timestamp_ms,
		.param = param,
	};
	int err;

	err = k_msgq_put(&g_event_queue, &event, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("Event drop: %s param=%d ts=%lld (err=%d)",
			app_event_type_str(type), param, (long long)timestamp_ms, err);
	} else if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS) && (type != APP_EVENT_TICK_1S)) {
		LOG_INF("Event pub: %s param=%d", app_event_type_str(type), param);
	}

	return err;
}

int app_event_publish(enum app_event_type type, int32_t param)
{
	return app_event_publish_with_timestamp(type, param, k_uptime_get());
}

bool app_event_wait(struct app_event *event, k_timeout_t timeout)
{
	return k_msgq_get(&g_event_queue, event, timeout) == 0;
}

const char *app_event_type_str(enum app_event_type type)
{
	if ((type < 0) || (type >= APP_EVENT_COUNT) || (g_event_names[type] == NULL)) {
		return "UNKNOWN";
	}

	return g_event_names[type];
}
