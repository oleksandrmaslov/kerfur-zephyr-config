#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/app_event.h"
#include "core/event_bus.h"

LOG_MODULE_REGISTER(event_bus, CONFIG_LOG_DEFAULT_LEVEL);

#define APP_EVENT_QUEUE_LEN 128

K_MSGQ_DEFINE(g_event_queue, sizeof(struct app_event), APP_EVENT_QUEUE_LEN, 4);

static bool event_is_periodic_tick(enum app_event_type type)
{
	return (type == APP_EVENT_TICK_100MS) ||
	       (type == APP_EVENT_TICK_1S) ||
	       (type == APP_EVENT_TICK_10S) ||
	       (type == APP_EVENT_TICK_60S);
}

static const char *const g_event_names[APP_EVENT_COUNT] = {
	[APP_EVENT_TICK_100MS] = "TICK_100MS",
	[APP_EVENT_TICK_1S] = "TICK_1S",
	[APP_EVENT_TICK_10S] = "TICK_10S",
	[APP_EVENT_TICK_60S] = "TICK_60S",
	[APP_EVENT_USER_TAP] = "USER_TAP",
	[APP_EVENT_USER_PET_SOFT] = "USER_PET_SOFT",
	[APP_EVENT_USER_PET_LONG] = "USER_PET_LONG",
	[APP_EVENT_USER_HOLD] = "USER_HOLD",
	[APP_EVENT_MOTION_WAKE] = "MOTION_WAKE",
	[APP_EVENT_WALKING_START] = "WALKING_START",
	[APP_EVENT_WALKING_STOP] = "WALKING_STOP",
	[APP_EVENT_STEP_BATCH] = "STEP_BATCH",
	[APP_EVENT_SHAKE_LIGHT] = "SHAKE_LIGHT",
	[APP_EVENT_SHAKE_PLAY] = "SHAKE_PLAY",
	[APP_EVENT_SHAKE_ROUGH] = "SHAKE_ROUGH",
	[APP_EVENT_FLIP_FACE_DOWN] = "FLIP_FACE_DOWN",
	[APP_EVENT_IMPACT] = "IMPACT",
	[APP_EVENT_PHONE_CONNECTED] = "PHONE_CONNECTED",
	[APP_EVENT_PHONE_DISCONNECTED] = "PHONE_DISCONNECTED",
	[APP_EVENT_PHONE_NOTIFICATION_SINGLE] = "PHONE_NOTIFICATION_SINGLE",
	[APP_EVENT_PHONE_NOTIFICATION_BURST] = "PHONE_NOTIFICATION_BURST",
	[APP_EVENT_APP_SESSION_START] = "APP_SESSION_START",
	[APP_EVENT_APP_SESSION_END] = "APP_SESSION_END",
	[APP_EVENT_TIME_SYNC] = "TIME_SYNC",
	[APP_EVENT_CHARGER_CONNECTED] = "CHARGER_CONNECTED",
	[APP_EVENT_CHARGER_DISCONNECTED] = "CHARGER_DISCONNECTED",
	[APP_EVENT_BATTERY_LOW] = "BATTERY_LOW",
	[APP_EVENT_BATTERY_CRITICAL] = "BATTERY_CRITICAL",
	[APP_EVENT_SELF_WAKE_TIMER] = "SELF_WAKE_TIMER",
	[APP_EVENT_IDLE_TIMEOUT] = "IDLE_TIMEOUT",
	[APP_EVENT_WAKE] = "WAKE",
	[APP_EVENT_SLEEP_REQUEST] = "SLEEP_REQUEST",
	[APP_EVENT_DISPLAY_FOREGROUND_TIMEOUT] = "DISPLAY_FOREGROUND_TIMEOUT",
	[APP_EVENT_DISPLAY_AMBIENT_TIMEOUT] = "DISPLAY_AMBIENT_TIMEOUT",
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
		/* Periodic ticks are best-effort and can be dropped under load. */
		if (!event_is_periodic_tick(type)) {
			LOG_WRN("Event drop: %s param=%d ts=%lld (err=%d)",
				app_event_type_str(type), param, (long long)timestamp_ms, err);
		}
	} else if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS) &&
		   (type != APP_EVENT_TICK_100MS) &&
		   (type != APP_EVENT_TICK_1S)) {
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
