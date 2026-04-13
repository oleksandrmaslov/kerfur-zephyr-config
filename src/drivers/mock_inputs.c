#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "drivers/mock_inputs.h"

LOG_MODULE_REGISTER(mock_inputs, CONFIG_LOG_DEFAULT_LEVEL);

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
static uint8_t g_periodic_mock_phase;
#endif
static uint32_t g_tick_100ms_count;

static bool event_is_tick(enum app_event_type type)
{
	return (type == APP_EVENT_TICK_100MS) ||
	       (type == APP_EVENT_TICK_1S) ||
	       (type == APP_EVENT_TICK_10S) ||
	       (type == APP_EVENT_TICK_60S);
}

static void publish_with_best_effort(enum app_event_type type)
{
	if (app_event_publish(type, 0) != 0) {
		if (!event_is_tick(type)) {
			LOG_WRN("Dropped event %s (queue full)", app_event_type_str(type));
		}
	}
}

static void tick_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	g_tick_100ms_count++;
	publish_with_best_effort(APP_EVENT_TICK_100MS);

	if ((g_tick_100ms_count % 10U) == 0U) {
		publish_with_best_effort(APP_EVENT_TICK_1S);
	}

	if ((g_tick_100ms_count % 100U) == 0U) {
		publish_with_best_effort(APP_EVENT_TICK_10S);
	}

	if ((g_tick_100ms_count % 600U) == 0U) {
		publish_with_best_effort(APP_EVENT_TICK_60S);
	}

	if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS)) {
		LOG_DBG("Tick 100ms");
	}
}

K_TIMER_DEFINE(g_tick_timer, tick_timer_handler, NULL);

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
static void mock_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	g_periodic_mock_phase++;

	if ((g_periodic_mock_phase % 2U) == 0U) {
		publish_with_best_effort(APP_EVENT_PHONE_NOTIFICATION_SINGLE);
		LOG_INF("Mock timer -> PHONE_NOTIFICATION_SINGLE");
	} else {
		publish_with_best_effort(APP_EVENT_SHAKE_PLAY);
		LOG_INF("Mock timer -> SHAKE_PLAY");
	}
}

K_TIMER_DEFINE(g_mock_timer, mock_timer_handler, NULL);
#endif

int mock_inputs_init(void)
{
	g_tick_100ms_count = 0U;
	k_timer_start(&g_tick_timer, K_NO_WAIT, K_MSEC(100));

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
	/*
	 * Periodic synthetic events keep behavior visibly active in bring-up
	 * builds that still enable CONFIG_KERFUR_ENABLE_MOCK_INPUTS.
	 */
	k_timer_start(&g_mock_timer, K_SECONDS(12), K_SECONDS(18));
#endif

	LOG_INF("Mock timers initialized");
	return 0;
}
