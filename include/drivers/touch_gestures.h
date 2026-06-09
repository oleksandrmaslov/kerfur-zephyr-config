#ifndef KERFUR_TOUCH_GESTURES_H_
#define KERFUR_TOUCH_GESTURES_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Hardware-agnostic touch gesture engine.
 *
 * Input is the set of currently-touched zones (a channel bitmask) plus a
 * timestamp. Output is a stream of recognized gestures delivered through a
 * callback. This is deliberately decoupled from any specific touch chip: the
 * IQS7222A driver, the legacy single GPIO, or a unit test all feed it the same
 * way. Keeping it pure makes it testable off-target.
 *
 * Zones are assumed ordered 0..channel_count-1 along the petting surface so a
 * sweep across adjacent zones can be read as a stroke. The exact electrode
 * layout comes from the flex PCB design; until that is final, multi-zone
 * contact within one touch is treated as a stroke.
 *
 * NOTE: touch_gestures_update() must be called periodically (e.g. every tick),
 * not only on edges, so deferred decisions (single-tap vs double-tap) and
 * time-based gestures (long-press/hold) can fire on time.
 */

enum touch_gesture {
	TOUCH_GESTURE_NONE = 0,
	TOUCH_GESTURE_TAP,             /* brief touch + release */
	TOUCH_GESTURE_DOUBLE_TAP,      /* two taps within the double-tap window */
	TOUCH_GESTURE_LONG_PRESS,      /* held past the long-press threshold (one-shot) */
	TOUCH_GESTURE_HOLD,            /* still held well past long-press (one-shot) */
	TOUCH_GESTURE_STROKE,          /* one pet / sweep across the surface */
	TOUCH_GESTURE_REPEATED_STROKE, /* several strokes in quick succession */
	TOUCH_GESTURE_COUNT,
};

struct touch_gesture_config {
	uint8_t  channel_count;            /* touch zones (1..32) */
	uint16_t tap_max_ms;               /* max contact to count as a tap */
	uint16_t double_tap_window_ms;     /* max gap between two taps */
	uint16_t long_press_ms;            /* contact >= this -> LONG_PRESS */
	uint16_t hold_ms;                  /* contact >= this -> HOLD */
	uint16_t repeated_stroke_window_ms;/* window to chain strokes */
	uint8_t  repeated_stroke_count;    /* strokes within window -> REPEATED_STROKE */
};

typedef void (*touch_gesture_cb_t)(enum touch_gesture gesture, void *user_data);

/* Fills cfg with sensible defaults (single-zone friendly). */
void touch_gestures_default_config(struct touch_gesture_config *cfg);

void touch_gestures_init(const struct touch_gesture_config *cfg,
			 touch_gesture_cb_t cb, void *user_data, int64_t now_ms);

/* Feed the current set of touched zones. Call on change and periodically. */
void touch_gestures_update(uint32_t channel_mask, int64_t now_ms);

/* Convenience wrapper for single-channel (GPIO) sources. */
void touch_gestures_set(bool touched, int64_t now_ms);

/* True while a contact is in progress or a single-tap is still waiting to see
 * if it becomes a double-tap. Touch front-ends use this to know how long to
 * keep polling update() after the last edge so deferred/timed gestures fire. */
bool touch_gestures_pending(void);

const char *touch_gesture_str(enum touch_gesture gesture);

#endif /* KERFUR_TOUCH_GESTURES_H_ */
