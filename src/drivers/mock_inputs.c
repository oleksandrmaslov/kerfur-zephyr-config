#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "drivers/mock_inputs.h"

LOG_MODULE_REGISTER(mock_inputs, CONFIG_LOG_DEFAULT_LEVEL);

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
static uint8_t g_periodic_mock_phase;
#endif

static void publish_with_best_effort(enum app_event_type type)
{
	if (app_event_publish(type, 0) != 0) {
		LOG_WRN("Dropped event %s (queue full)", app_event_type_str(type));
	}
}

static void tick_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	publish_with_best_effort(APP_EVENT_TICK_1S);

	if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS)) {
		LOG_DBG("Tick 1s");
	}
}

K_TIMER_DEFINE(g_tick_timer, tick_timer_handler, NULL);

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
static void mock_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	g_periodic_mock_phase++;

	if ((g_periodic_mock_phase % 2U) == 0U) {
		publish_with_best_effort(APP_EVENT_MOCK_NOTIFICATION);
		LOG_INF("Mock timer -> MOCK_NOTIFICATION");
	} else {
		publish_with_best_effort(APP_EVENT_MOCK_SHAKE);
		LOG_INF("Mock timer -> MOCK_SHAKE");
	}
}

K_TIMER_DEFINE(g_mock_timer, mock_timer_handler, NULL);
#endif

int mock_inputs_init(void)
{
	k_timer_start(&g_tick_timer, K_NO_WAIT, K_SECONDS(1));

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
	/*
	 * Periodic synthetic events keep behavior visibly active before
	 * full sensor stack (touch, IMU, notifications) is integrated.
	 * TODO: Replace periodic synthetic events with real sensor/phone inputs.
	 */
	k_timer_start(&g_mock_timer, K_SECONDS(12), K_SECONDS(18));
#endif

	LOG_INF("Mock timers initialized");
	return 0;
}
