#include <stdbool.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include "core/event_bus.h"
#include "display/display_policy.h"

LOG_MODULE_REGISTER(display_policy, CONFIG_LOG_DEFAULT_LEVEL);

static const char *display_state_str(enum pet_display_state state)
{
	switch (state) {
	case DISPLAY_FOREGROUND:
		return "FOREGROUND";
	case DISPLAY_AMBIENT:
		return "AMBIENT";
	case DISPLAY_OFF:
		return "OFF";
	default:
		return "UNKNOWN";
	}
}

struct display_policy_runtime {
	int64_t foreground_deadline_ms;
	int64_t ambient_deadline_ms;
	int64_t next_self_wake_ms;
	int64_t app_session_last_activity_ms;
	bool last_reported_foreground_timeout;
	bool last_reported_ambient_timeout;
};

static struct display_policy_runtime g_policy;

static bool is_night_time(const struct pet_state *state)
{
	int64_t elapsed_s;
	int64_t local_s;
	int32_t day_s;
	int hour;

	if (!state->time_valid) {
		return false;
	}

	elapsed_s = (k_uptime_get() - state->uptime_at_sync_ms) / MSEC_PER_SEC;
	local_s = state->unix_time_at_sync + elapsed_s + ((int64_t)state->tz_offset_minutes * 60);
	day_s = (int32_t)(local_s % 86400);
	if (day_s < 0) {
		day_s += 86400;
	}
	hour = day_s / 3600;
	return (hour < 7) || (hour >= 22);
}

static int32_t jitter_range_ms(int32_t min_ms, int32_t max_ms)
{
	uint32_t span;
	uint32_t rnd;

	if (max_ms <= min_ms) {
		return min_ms;
	}

	span = (uint32_t)(max_ms - min_ms + 1);
	rnd = sys_rand32_get() % span;
	return min_ms + (int32_t)rnd;
}

static void schedule_self_wake(const struct pet_state *state, int64_t now_ms)
{
	int32_t wake_delay_ms;

	if (!state->ambient_wake_enabled || state->battery_critical) {
		g_policy.next_self_wake_ms = INT64_MAX;
		return;
	}

	if (state->charging) {
		wake_delay_ms = jitter_range_ms(5 * 60 * MSEC_PER_SEC, 15 * 60 * MSEC_PER_SEC);
	} else if (state->current_mode == PET_MODE_WALK_AWAKE) {
		wake_delay_ms = jitter_range_ms(2 * 60 * MSEC_PER_SEC, 5 * 60 * MSEC_PER_SEC);
	} else if ((state->current_expression == PET_EXPR_NEEDY) ||
		   (state->current_expression == PET_EXPR_LONELY)) {
		wake_delay_ms = jitter_range_ms(8 * 60 * MSEC_PER_SEC, 20 * 60 * MSEC_PER_SEC);
	} else if (state->battery_low) {
		wake_delay_ms = jitter_range_ms(30 * 60 * MSEC_PER_SEC, 90 * 60 * MSEC_PER_SEC);
	} else {
		wake_delay_ms = jitter_range_ms(20 * 60 * MSEC_PER_SEC, 60 * 60 * MSEC_PER_SEC);
	}

	g_policy.next_self_wake_ms = now_ms + wake_delay_ms;
}

static int32_t apply_event_modifiers_ms(const struct pet_state *state, int32_t base_ms, bool ambient)
{
	int32_t tuned = base_ms;

	if (is_night_time(state)) {
		tuned = (tuned * 7) / 10;
	}

	if (ambient && state->battery_low) {
		tuned /= 2;
	}

	if (state->battery_critical) {
		if (ambient) {
			tuned = 0;
		} else {
			tuned = MIN(tuned, 6000);
		}
	}

	if (tuned < 0) {
		tuned = 0;
	}

	return tuned;
}

static void set_display_state(struct pet_state *state, enum pet_display_state next_state, int64_t now_ms)
{
	const enum pet_display_state prev_state = state->current_display_state;

	if (state->current_display_state == next_state) {
		return;
	}

	state->current_display_state = next_state;
	state->last_display_state_change_ms = now_ms;
	LOG_INF("Display: %s -> %s",
		display_state_str(prev_state),
		display_state_str(next_state));
}

static void set_foreground_window(struct pet_state *state, int64_t now_ms, int32_t fg_ms, int32_t ambient_ms)
{
	fg_ms = apply_event_modifiers_ms(state, fg_ms, false);
	ambient_ms = apply_event_modifiers_ms(state, ambient_ms, true);

	set_display_state(state, DISPLAY_FOREGROUND, now_ms);
	g_policy.foreground_deadline_ms = now_ms + fg_ms;
	g_policy.ambient_deadline_ms = g_policy.foreground_deadline_ms + ambient_ms;
	g_policy.last_reported_foreground_timeout = false;
	g_policy.last_reported_ambient_timeout = false;
}

static void set_ambient_window(struct pet_state *state, int64_t now_ms, int32_t ambient_ms)
{
	ambient_ms = apply_event_modifiers_ms(state, ambient_ms, true);

	set_display_state(state, DISPLAY_AMBIENT, now_ms);
	g_policy.ambient_deadline_ms = now_ms + ambient_ms;
	g_policy.last_reported_foreground_timeout = true;
	g_policy.last_reported_ambient_timeout = false;
}

void display_policy_init(struct pet_state *state, int64_t now_ms)
{
	g_policy.foreground_deadline_ms = now_ms + (10 * MSEC_PER_SEC);
	g_policy.ambient_deadline_ms = now_ms + (40 * MSEC_PER_SEC);
	g_policy.app_session_last_activity_ms = now_ms;
	g_policy.last_reported_foreground_timeout = false;
	g_policy.last_reported_ambient_timeout = false;
	state->current_display_state = DISPLAY_FOREGROUND;
	state->last_display_state_change_ms = now_ms;
	state->ambient_wake_enabled = true;
	schedule_self_wake(state, now_ms);
}

void display_policy_on_event(struct pet_state *state, const struct app_event *event)
{
	const int64_t now_ms = event->timestamp_ms;
	const bool asleep_path = (state->current_mode == PET_MODE_ASLEEP) ||
				 (state->current_display_state == DISPLAY_OFF);

	switch (event->type) {
	case APP_EVENT_PHONE_CONNECTED:
		set_foreground_window(state, now_ms, 10 * MSEC_PER_SEC, 45 * MSEC_PER_SEC);
		break;
	case APP_EVENT_PHONE_DISCONNECTED:
		set_foreground_window(state, now_ms, 8 * MSEC_PER_SEC, 25 * MSEC_PER_SEC);
		break;
	case APP_EVENT_USER_TAP:
		set_foreground_window(state, now_ms, 10 * MSEC_PER_SEC, 30 * MSEC_PER_SEC);
		break;
	case APP_EVENT_USER_PET_SOFT:
		set_foreground_window(state, now_ms, 20 * MSEC_PER_SEC, 90 * MSEC_PER_SEC);
		break;
	case APP_EVENT_USER_PET_LONG:
	case APP_EVENT_USER_HOLD:
		set_foreground_window(state, now_ms, 30 * MSEC_PER_SEC, 120 * MSEC_PER_SEC);
		break;
	case APP_EVENT_PHONE_NOTIFICATION_SINGLE:
		if (asleep_path) {
			set_foreground_window(state, now_ms, 6 * MSEC_PER_SEC, 25 * MSEC_PER_SEC);
		} else {
			set_foreground_window(state, now_ms, 10 * MSEC_PER_SEC, 45 * MSEC_PER_SEC);
		}
		break;
	case APP_EVENT_PHONE_NOTIFICATION_BURST:
		set_foreground_window(state, now_ms, 16 * MSEC_PER_SEC, 45 * MSEC_PER_SEC);
		break;
	case APP_EVENT_CHARGER_CONNECTED:
		set_foreground_window(state, now_ms, 30 * MSEC_PER_SEC, 600 * MSEC_PER_SEC);
		break;
	case APP_EVENT_WALKING_START:
		set_foreground_window(state, now_ms, 20 * MSEC_PER_SEC, 180 * MSEC_PER_SEC);
		break;
	case APP_EVENT_APP_SESSION_START:
		g_policy.app_session_last_activity_ms = now_ms;
		set_foreground_window(state, now_ms, 40 * MSEC_PER_SEC, 10 * 60 * MSEC_PER_SEC);
		break;
	case APP_EVENT_APP_SESSION_END:
		g_policy.app_session_last_activity_ms = now_ms;
		break;
	case APP_EVENT_BATTERY_CRITICAL:
		state->ambient_wake_enabled = false;
		g_policy.next_self_wake_ms = INT64_MAX;
		if (state->current_display_state == DISPLAY_AMBIENT) {
			set_display_state(state, DISPLAY_OFF, now_ms);
			g_policy.ambient_deadline_ms = now_ms;
		}
		break;
	case APP_EVENT_BATTERY_LOW:
		if (state->current_display_state == DISPLAY_AMBIENT) {
			g_policy.ambient_deadline_ms = now_ms + ((g_policy.ambient_deadline_ms - now_ms) / 2);
		}
		break;
	case APP_EVENT_WAKE:
		set_foreground_window(state, now_ms, 8 * MSEC_PER_SEC, 30 * MSEC_PER_SEC);
		break;
	case APP_EVENT_SELF_WAKE_TIMER:
		if (state->battery_critical) {
			break;
		}
		state->last_self_wake_timestamp_ms = now_ms;
		set_foreground_window(state, now_ms, 6 * MSEC_PER_SEC, 20 * MSEC_PER_SEC);
		break;
	case APP_EVENT_TICK_100MS:
		display_policy_on_tick_100ms(state, now_ms);
		break;
	default:
		break;
	}

	if ((event->type == APP_EVENT_USER_TAP) || (event->type == APP_EVENT_USER_PET_SOFT) ||
	    (event->type == APP_EVENT_USER_PET_LONG) || (event->type == APP_EVENT_USER_HOLD) ||
	    (event->type == APP_EVENT_PHONE_CONNECTED) ||
	    (event->type == APP_EVENT_PHONE_DISCONNECTED) ||
	    (event->type == APP_EVENT_CHARGER_CONNECTED) || (event->type == APP_EVENT_PHONE_NOTIFICATION_SINGLE) ||
	    (event->type == APP_EVENT_PHONE_NOTIFICATION_BURST)) {
		g_policy.app_session_last_activity_ms = now_ms;
		schedule_self_wake(state, now_ms);
	}
}

void display_policy_on_tick_100ms(struct pet_state *state, int64_t now_ms)
{
	if (state->app_session_active && !state->battery_critical) {
		if ((state->current_display_state == DISPLAY_FOREGROUND) &&
		    ((now_ms - g_policy.app_session_last_activity_ms) >= (60 * MSEC_PER_SEC))) {
			set_ambient_window(state, now_ms, 10 * 60 * MSEC_PER_SEC);
			(void)app_event_publish(APP_EVENT_DISPLAY_FOREGROUND_TIMEOUT, 0);
		}
		g_policy.ambient_deadline_ms = now_ms + (10 * 60 * MSEC_PER_SEC);
	}

	if ((state->current_display_state == DISPLAY_FOREGROUND) &&
	    (now_ms >= g_policy.foreground_deadline_ms)) {
		set_display_state(state, DISPLAY_AMBIENT, now_ms);
		if (!g_policy.last_reported_foreground_timeout) {
			(void)app_event_publish(APP_EVENT_DISPLAY_FOREGROUND_TIMEOUT, 0);
			g_policy.last_reported_foreground_timeout = true;
		}
	}

	if ((state->current_display_state == DISPLAY_AMBIENT) &&
	    (now_ms >= g_policy.ambient_deadline_ms)) {
		if (!(state->app_session_active && !state->battery_critical)) {
			set_display_state(state, DISPLAY_OFF, now_ms);
			if (!g_policy.last_reported_ambient_timeout) {
				(void)app_event_publish(APP_EVENT_DISPLAY_AMBIENT_TIMEOUT, 0);
				g_policy.last_reported_ambient_timeout = true;
			}
		}
	}

	if (!state->battery_critical && (state->current_display_state == DISPLAY_OFF) &&
	    (now_ms >= g_policy.next_self_wake_ms)) {
		/*
		 * Wake display directly so queue pressure cannot suppress visible self-wake.
		 * We still publish the event for behavior/reaction updates.
		 */
		set_foreground_window(state, now_ms, 6 * MSEC_PER_SEC, 20 * MSEC_PER_SEC);
		state->last_self_wake_timestamp_ms = now_ms;
		(void)app_event_publish(APP_EVENT_SELF_WAKE_TIMER, 0);
		schedule_self_wake(state, now_ms);
	}
}

enum pet_display_state display_policy_get_state(const struct pet_state *state)
{
	return state->current_display_state;
}
