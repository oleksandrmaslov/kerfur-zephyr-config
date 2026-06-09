/*
 * Hardware-agnostic touch gesture engine. See touch_gestures.h.
 *
 * Pipeline per touch contact (rising edge -> hold -> falling edge):
 *   - while held: fire LONG_PRESS then HOLD as time thresholds pass (one-shot)
 *   - on release, classify by duration + how many zones were involved:
 *       multi-zone contact .................. STROKE (a sweep / pet)
 *       single-zone, <= tap_max ............. TAP (deferred for double-tap)
 *       single-zone, tap_max..long_press .... STROKE (a single-zone pet)
 *       single-zone, >= long_press .......... already emitted as LONG_PRESS/HOLD
 *   - two taps within the double-tap window . DOUBLE_TAP
 *   - N strokes within the repeat window .... REPEATED_STROKE (instead of STROKE)
 *
 * A deferred single TAP is flushed once the double-tap window lapses, which is
 * why update() must be polled, not just edge-driven.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include "drivers/touch_gestures.h"

struct touch_gestures_state {
	struct touch_gesture_config cfg;
	touch_gesture_cb_t cb;
	void *user;

	bool in_contact;
	int64_t contact_start_ms;
	uint32_t zones_touched_mask; /* union of zones seen during this contact */
	bool long_fired;
	bool hold_fired;

	bool pending_tap;
	int64_t pending_tap_ms;

	uint8_t stroke_count;
	int64_t last_stroke_ms;
};

static struct touch_gestures_state g_tg;

static void emit(enum touch_gesture gesture)
{
	if (g_tg.cb != NULL && gesture != TOUCH_GESTURE_NONE) {
		g_tg.cb(gesture, g_tg.user);
	}
}

void touch_gestures_default_config(struct touch_gesture_config *cfg)
{
	if (cfg == NULL) {
		return;
	}
	cfg->channel_count = 1U;
	cfg->tap_max_ms = 250U;
	cfg->double_tap_window_ms = 400U;
	cfg->long_press_ms = 700U;
	cfg->hold_ms = 1500U;
	cfg->repeated_stroke_window_ms = 1200U;
	cfg->repeated_stroke_count = 3U;
}

void touch_gestures_init(const struct touch_gesture_config *cfg,
			 touch_gesture_cb_t cb, void *user_data, int64_t now_ms)
{
	memset(&g_tg, 0, sizeof(g_tg));

	if (cfg != NULL) {
		g_tg.cfg = *cfg;
	} else {
		touch_gestures_default_config(&g_tg.cfg);
	}
	if (g_tg.cfg.channel_count == 0U) {
		g_tg.cfg.channel_count = 1U;
	}

	g_tg.cb = cb;
	g_tg.user = user_data;
	g_tg.last_stroke_ms = now_ms;
}

static void handle_stroke(int64_t now_ms)
{
	if ((now_ms - g_tg.last_stroke_ms) <= (int64_t)g_tg.cfg.repeated_stroke_window_ms) {
		if (g_tg.stroke_count < UINT8_MAX) {
			g_tg.stroke_count++;
		}
	} else {
		g_tg.stroke_count = 1U;
	}
	g_tg.last_stroke_ms = now_ms;

	if (g_tg.cfg.repeated_stroke_count > 0U &&
	    g_tg.stroke_count >= g_tg.cfg.repeated_stroke_count) {
		g_tg.stroke_count = 0U; /* require a fresh run for the next one */
		emit(TOUCH_GESTURE_REPEATED_STROKE);
	} else {
		emit(TOUCH_GESTURE_STROKE);
	}
}

static void on_release(int64_t now_ms)
{
	const int64_t dur = now_ms - g_tg.contact_start_ms;
	const bool multi_zone = (__builtin_popcount(g_tg.zones_touched_mask) >= 2);

	g_tg.in_contact = false;
	g_tg.zones_touched_mask = 0U;

	if (g_tg.long_fired) {
		/* LONG_PRESS / HOLD already reported; release ends it. */
		return;
	}

	if (multi_zone) {
		handle_stroke(now_ms);
		return;
	}

	if (dur <= (int64_t)g_tg.cfg.tap_max_ms) {
		if (g_tg.pending_tap &&
		    (now_ms - g_tg.pending_tap_ms) <= (int64_t)g_tg.cfg.double_tap_window_ms) {
			g_tg.pending_tap = false;
			emit(TOUCH_GESTURE_DOUBLE_TAP);
		} else {
			g_tg.pending_tap = true;
			g_tg.pending_tap_ms = now_ms;
		}
		return;
	}

	/* Single zone, longer than a tap but short of a long-press: a pet. */
	handle_stroke(now_ms);
}

void touch_gestures_update(uint32_t channel_mask, int64_t now_ms)
{
	const bool active = (channel_mask != 0U);

	if (active && !g_tg.in_contact) {
		/* Rising edge: start a new contact. */
		g_tg.in_contact = true;
		g_tg.contact_start_ms = now_ms;
		g_tg.zones_touched_mask = channel_mask;
		g_tg.long_fired = false;
		g_tg.hold_fired = false;
	} else if (active && g_tg.in_contact) {
		const int64_t dur = now_ms - g_tg.contact_start_ms;

		g_tg.zones_touched_mask |= channel_mask;

		if (!g_tg.long_fired && dur >= (int64_t)g_tg.cfg.long_press_ms) {
			g_tg.long_fired = true;
			emit(TOUCH_GESTURE_LONG_PRESS);
		}
		if (!g_tg.hold_fired && dur >= (int64_t)g_tg.cfg.hold_ms) {
			g_tg.hold_fired = true;
			emit(TOUCH_GESTURE_HOLD);
		}
	} else if (!active && g_tg.in_contact) {
		on_release(now_ms);
	}

	/* Flush a deferred single tap once the double-tap window has lapsed. */
	if (!g_tg.in_contact && g_tg.pending_tap &&
	    (now_ms - g_tg.pending_tap_ms) > (int64_t)g_tg.cfg.double_tap_window_ms) {
		g_tg.pending_tap = false;
		emit(TOUCH_GESTURE_TAP);
	}
}

void touch_gestures_set(bool touched, int64_t now_ms)
{
	touch_gestures_update(touched ? 1U : 0U, now_ms);
}

bool touch_gestures_pending(void)
{
	return g_tg.in_contact || g_tg.pending_tap;
}

const char *touch_gesture_str(enum touch_gesture gesture)
{
	switch (gesture) {
	case TOUCH_GESTURE_NONE:            return "NONE";
	case TOUCH_GESTURE_TAP:             return "TAP";
	case TOUCH_GESTURE_DOUBLE_TAP:      return "DOUBLE_TAP";
	case TOUCH_GESTURE_LONG_PRESS:      return "LONG_PRESS";
	case TOUCH_GESTURE_HOLD:            return "HOLD";
	case TOUCH_GESTURE_STROKE:          return "STROKE";
	case TOUCH_GESTURE_REPEATED_STROKE: return "REPEATED_STROKE";
	default:                            return "?";
	}
}
