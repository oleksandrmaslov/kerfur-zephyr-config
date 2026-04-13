#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "power/power_manager.h"

LOG_MODULE_REGISTER(power_manager, CONFIG_LOG_DEFAULT_LEVEL);

struct power_state {
	int64_t last_real_interaction_ms;
	bool idle_timeout_sent;
	bool sleep_request_sent;
};

static struct power_state g_power = {
	.last_real_interaction_ms = 0,
	.idle_timeout_sent = false,
	.sleep_request_sent = false,
};

static bool event_is_real_interaction(enum app_event_type type)
{
	switch (type) {
	case APP_EVENT_USER_TAP:
	case APP_EVENT_USER_PET_SOFT:
	case APP_EVENT_USER_PET_LONG:
	case APP_EVENT_USER_HOLD:
	case APP_EVENT_PICKED_UP:
	case APP_EVENT_IN_HAND_ENTER:
	case APP_EVENT_APP_SESSION_START:
	case APP_EVENT_CHARGER_CONNECTED:
	case APP_EVENT_WAKE:
		return true;
	default:
		return false;
	}
}

void power_manager_init(int64_t now_ms)
{
	g_power.last_real_interaction_ms = now_ms;
	g_power.idle_timeout_sent = false;
	g_power.sleep_request_sent = false;
	LOG_INF("Power manager init: idle=%ds blank=%ds sleep=%ds",
		CONFIG_KERFUR_IDLE_TIMEOUT_S, CONFIG_KERFUR_DISPLAY_BLANK_TIMEOUT_S,
		CONFIG_KERFUR_SLEEP_REQUEST_TIMEOUT_S);
}

void power_manager_on_event(const struct app_event *event, const struct pet_state *state)
{
	ARG_UNUSED(state);

	if (event_is_real_interaction(event->type)) {
		g_power.last_real_interaction_ms = event->timestamp_ms;
		g_power.idle_timeout_sent = false;
		g_power.sleep_request_sent = false;
		LOG_INF("Power reset by interaction (%s)", app_event_type_str(event->type));
	}

	if (event->type == APP_EVENT_SLEEP_REQUEST) {
		g_power.sleep_request_sent = true;
		LOG_INF("Power sleep request acknowledged");
	}
}

bool power_manager_poll(int64_t now_ms, struct app_event *out_event)
{
	int64_t idle_ms = now_ms - g_power.last_real_interaction_ms;

	if (!g_power.idle_timeout_sent &&
	    idle_ms >= ((int64_t)CONFIG_KERFUR_IDLE_TIMEOUT_S * MSEC_PER_SEC)) {
		g_power.idle_timeout_sent = true;
		*out_event = (struct app_event){0};
		out_event->type = APP_EVENT_IDLE_TIMEOUT;
		out_event->timestamp_ms = now_ms;
		out_event->param = 0;
		LOG_INF("Power emitted IDLE_TIMEOUT after %lld ms", (long long)idle_ms);
		return true;
	}

	if (!g_power.sleep_request_sent &&
	    idle_ms >= ((int64_t)CONFIG_KERFUR_SLEEP_REQUEST_TIMEOUT_S * MSEC_PER_SEC)) {
		/* TODO: Add real deep-sleep entry policy and wake source negotiation. */
		g_power.sleep_request_sent = true;
		*out_event = (struct app_event){0};
		out_event->type = APP_EVENT_SLEEP_REQUEST;
		out_event->timestamp_ms = now_ms;
		out_event->param = 0;
		LOG_INF("Power emitted SLEEP_REQUEST after %lld ms", (long long)idle_ms);
		return true;
	}

	return false;
}

bool power_manager_is_display_blanked(void)
{
	/* Display blanking is owned by display_policy in phase 1. */
	return false;
}
