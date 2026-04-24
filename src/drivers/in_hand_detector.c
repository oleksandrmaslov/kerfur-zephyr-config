#include <string.h>

#include <zephyr/sys/util.h>

#include "drivers/in_hand_detector.h"

/*
 * The detector is intentionally small: the classifier provides cleaned,
 * windowed evidence and this module keeps hysteresis, surface memory and
 * one-shot pickup/in-hand events.
 */

#define PICKUP_CANDIDATE_THRESHOLD 35U
#define PICKED_UP_THRESHOLD        52U
#define IN_HAND_ENTER_THRESHOLD    70U
#define IN_HAND_EXIT_THRESHOLD     24U

#define SURFACE_STILL_CONFIRM_MS       1400LL
#define SURFACE_STILL_RECENT_MS        3200LL
#define PICKUP_RECENT_MS               6000LL
#define IN_HAND_EXIT_CONFIRM_MS        900LL
#define IN_HAND_EXIT_HOLD_AFTER_ENTER  1400LL

#define CONF_Q8_MAX (100 * 256)

static uint8_t clamp_u8(int value)
{
	return (uint8_t)CLAMP(value, 0, 100);
}

static int32_t clamp_conf_q8(int32_t value)
{
	return CLAMP(value, 0, CONF_Q8_MAX);
}

static uint8_t conf_q8_to_u8(int32_t value)
{
	return clamp_u8((int)((value + 128) / 256));
}

static int32_t confidence_delta_q8(int rate_per_s, int32_t dt_ms)
{
	int64_t delta;

	dt_ms = CLAMP(dt_ms, 10, 200);
	delta = (int64_t)rate_per_s * 256LL * (int64_t)dt_ms;
	return (int32_t)(delta / 1000LL);
}

static void adjust_confidence(int32_t *confidence_q8, int rate_per_s, int32_t dt_ms)
{
	*confidence_q8 = clamp_conf_q8(*confidence_q8 +
				       confidence_delta_q8(rate_per_s, dt_ms));
}

static int abs_i32(int value)
{
	return (value < 0) ? -value : value;
}

static uint16_t gravity_distance(const struct in_hand_detector *det,
				 const struct in_hand_detector_input *in)
{
	return (uint16_t)(abs_i32(in->gravity_x - det->surface_gravity_x) +
			  abs_i32(in->gravity_y - det->surface_gravity_y) +
			  abs_i32(in->gravity_z - det->surface_gravity_z));
}

static bool surface_now(const struct in_hand_detector_input *in)
{
	return !in->rough_motion &&
	       !in->walking_active &&
	       in->walking_confidence < 35U &&
	       in->cadence_confidence < 35U &&
	       in->surface_still_confidence >= 78U &&
	       in->smooth_motion_mg <= 55U &&
	       in->orientation_rate_mg <= 120U &&
	       in->gyro_sum_mdps <= 10000U;
}

static bool surface_confirmed(const struct in_hand_detector *det,
			      const struct in_hand_detector_input *in)
{
	return det->surface_still_armed &&
	       ((in->now_ms - det->still_since_ms) >= SURFACE_STILL_CONFIRM_MS);
}

static bool had_surface_recently(const struct in_hand_detector *det,
				 const struct in_hand_detector_input *in)
{
	return (det->last_surface_leave_ms > 0LL) &&
	       ((in->now_ms - det->last_surface_leave_ms) <= SURFACE_STILL_RECENT_MS);
}

static bool tiny_table_vibration(const struct in_hand_detector_input *in)
{
	return in->surface_still_confidence >= 65U &&
	       in->rotation_confidence <= 20U &&
	       in->hand_motion_confidence <= 25U &&
	       in->smooth_motion_mg <= 70U &&
	       in->gyro_sum_mdps <= 9000U;
}

static bool hand_like_motion(const struct in_hand_detector_input *in)
{
	if (in->rough_motion || in->chaos_confidence >= 72U) {
		return false;
	}
	if (in->walking_confidence >= 65U || in->cadence_confidence >= 65U) {
		return false;
	}
	if (tiny_table_vibration(in)) {
		return false;
	}

	return in->hand_motion_confidence >= 44U ||
	       (in->rotation_confidence >= 38U && in->motion_mg >= 12U) ||
	       (in->orientation_rate_mg >= 120U && in->gyro_sum_mdps >= 2500U &&
		in->smooth_motion_mg >= 18U);
}

static bool smooth_pickup_motion(const struct in_hand_detector_input *in)
{
	return hand_like_motion(in) &&
	       in->chaos_confidence <= 58U &&
	       in->cadence_confidence < 50U &&
	       in->motion_mg <= 420U &&
	       in->jerk_mg <= 5500U &&
	       in->gyro_sum_mdps <= 45000U;
}

static int pickup_evidence_rate(const struct in_hand_detector *det,
				const struct in_hand_detector_input *in,
				bool surface_recent)
{
	int rate = -18;

	if (surface_now(in)) {
		return -38;
	}
	if (in->rough_motion || in->chaos_confidence >= 78U) {
		return -55;
	}
	if (in->walking_active || in->cadence_confidence >= 65U) {
		return -34;
	}
	if (!smooth_pickup_motion(in)) {
		return det->in_hand ? -6 : -20;
	}

	if (surface_recent || in->motion_wake || det->pickup_confidence >= 20U) {
		rate = 58;
		rate += (int)in->hand_motion_confidence / 3;
		rate += (int)in->rotation_confidence / 4;
		rate += (int)in->stability_confidence / 5;
		rate -= (int)in->chaos_confidence / 4;
	}

	return CLAMP(rate, -70, 95);
}

static int in_hand_evidence_rate(const struct in_hand_detector *det,
				 const struct in_hand_detector_input *in,
				 bool surface_recent, bool surface_ok)
{
	int rate;
	const bool hand_like = hand_like_motion(in);

	if (surface_ok) {
		return det->in_hand ? -46 : -60;
	}
	if (in->rough_motion) {
		return det->in_hand ? -18 : -42;
	}
	if (in->walking_active || in->cadence_confidence >= 70U) {
		return det->in_hand ? -8 : -30;
	}
	if (!hand_like) {
		if (det->in_hand) {
			return tiny_table_vibration(in) ? -24 : -7;
		}
		return -18;
	}

	rate = 42;
	if (surface_recent || det->pickup_confidence >= 32U) {
		rate += 18;
	}
	rate += (int)in->hand_motion_confidence / 3;
	rate += (int)in->rotation_confidence / 5;
	rate += (int)in->stability_confidence / 8;
	rate -= (int)in->chaos_confidence / 5;
	rate -= (int)in->cadence_confidence / 6;

	return CLAMP(rate, -45, 95);
}

void in_hand_detector_init(struct in_hand_detector *det, int64_t now_ms)
{
	if (det == NULL) {
		return;
	}

	memset(det, 0, sizeof(*det));
	det->state = IN_HAND_DETECTOR_SURFACE_STILL;
	det->still_since_ms = now_ms;
}

void in_hand_detector_process(struct in_hand_detector *det,
			      const struct in_hand_detector_input *in,
			      struct in_hand_detector_output *out)
{
	bool surface_still;
	bool surface_ok;
	bool surface_recent;
	bool hand_like;
	bool pickup_like;
	bool gravity_moved;
	bool exit_ready;
	int pickup_rate;
	int hand_rate;

	if (det == NULL || in == NULL || out == NULL) {
		return;
	}

	memset(out, 0, sizeof(*out));

	surface_still = surface_now(in);
	if (surface_still) {
		if (!det->surface_still_armed) {
			det->still_since_ms = in->now_ms;
			det->surface_still_armed = true;
		}
		det->surface_gravity_x = in->gravity_x;
		det->surface_gravity_y = in->gravity_y;
		det->surface_gravity_z = in->gravity_z;
	} else {
		if (det->surface_still_armed) {
			det->last_surface_leave_ms = in->now_ms;
		}
		det->surface_still_armed = false;
	}

	surface_ok = surface_confirmed(det, in);
	surface_recent = had_surface_recently(det, in);
	hand_like = hand_like_motion(in);
	pickup_like = smooth_pickup_motion(in);
	gravity_moved = gravity_distance(det, in) >= 115U;

	pickup_rate = pickup_evidence_rate(det, in, surface_recent);
	if (pickup_like && gravity_moved) {
		pickup_rate += 14;
	}
	if (pickup_like && (surface_recent || in->motion_wake)) {
		pickup_rate += 10;
	}
	adjust_confidence(&det->pickup_confidence_q8, pickup_rate, in->dt_ms);

	hand_rate = in_hand_evidence_rate(det, in, surface_recent, surface_ok);
	if (hand_like && gravity_moved) {
		hand_rate += 10;
	}
	if (det->picked_up_reported && hand_like) {
		hand_rate += 8;
	}
	adjust_confidence(&det->in_hand_confidence_q8, hand_rate, in->dt_ms);

	det->pickup_confidence = conf_q8_to_u8(det->pickup_confidence_q8);
	det->in_hand_confidence = conf_q8_to_u8(det->in_hand_confidence_q8);

	if (!det->in_hand) {
		if (det->pickup_candidate_reported &&
		    det->pickup_confidence < (PICKUP_CANDIDATE_THRESHOLD - 8U)) {
			det->pickup_candidate_reported = false;
		}
		if (det->picked_up_reported &&
		    (det->pickup_confidence <= 24U ||
		     (surface_ok && det->in_hand_confidence <= 22U))) {
			det->picked_up_reported = false;
		}
	}

	if (!det->pickup_candidate_reported &&
	    det->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
		out->pickup_candidate = true;
		det->pickup_candidate_reported = true;
	}

	if (!det->picked_up_reported &&
	    det->pickup_confidence >= PICKED_UP_THRESHOLD &&
	    det->in_hand_confidence >= 38U &&
	    pickup_like) {
		out->picked_up = true;
		det->picked_up_reported = true;
		det->last_pickup_ms = in->now_ms;
	}

	if (!det->in_hand && det->in_hand_confidence >= IN_HAND_ENTER_THRESHOLD) {
		det->in_hand = true;
		det->state = IN_HAND_DETECTOR_IN_HAND;
		det->last_in_hand_ms = in->now_ms;
		det->exit_candidate_since_ms = 0LL;
		out->in_hand_enter = true;
	} else if (det->in_hand) {
		exit_ready = surface_ok ||
			     det->in_hand_confidence <= IN_HAND_EXIT_THRESHOLD;
		if (exit_ready) {
			if (det->exit_candidate_since_ms == 0LL) {
				det->exit_candidate_since_ms = in->now_ms;
			}
			if (((in->now_ms - det->last_in_hand_ms) >=
			     IN_HAND_EXIT_HOLD_AFTER_ENTER) &&
			    ((in->now_ms - det->exit_candidate_since_ms) >=
			     IN_HAND_EXIT_CONFIRM_MS)) {
				det->in_hand = false;
				det->exit_candidate_since_ms = 0LL;
				out->in_hand_exit = true;
				if (surface_ok) {
					det->state = IN_HAND_DETECTOR_SURFACE_STILL;
				}
			}
		} else {
			det->exit_candidate_since_ms = 0LL;
		}
	}

	if (det->in_hand) {
		det->state = in->walking_active ? IN_HAND_DETECTOR_WALKING :
						  IN_HAND_DETECTOR_IN_HAND;
	} else if (in->rough_motion) {
		det->state = IN_HAND_DETECTOR_SHAKE_EVENT;
	} else if (det->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
		det->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	} else if (surface_ok) {
		det->state = IN_HAND_DETECTOR_SURFACE_STILL;
	}

	out->look_confidence = 0U;
	if (det->in_hand && !in->rough_motion) {
		int look = (int)det->in_hand_confidence;

		look += (int)in->stability_confidence / 3;
		look += (int)in->rotation_confidence / 8;
		look -= (int)in->chaos_confidence / 2;
		look -= (int)in->cadence_confidence / 3;
		if (in->walking_active) {
			look /= 2;
		}
		out->look_confidence = clamp_u8(look);
	} else if (det->state == IN_HAND_DETECTOR_MAYBE_PICKED_UP) {
		out->look_confidence = clamp_u8(
			(int)det->pickup_confidence / 2 +
			(int)in->rotation_confidence / 4 -
			(int)in->chaos_confidence / 5);
	}

	out->state = det->state;
	out->pickup_confidence = det->pickup_confidence;
	out->in_hand_confidence = det->in_hand_confidence;
	out->picked_up_recently =
		(det->last_pickup_ms > 0LL) &&
		((in->now_ms - det->last_pickup_ms) <= PICKUP_RECENT_MS);
	out->in_hand = det->in_hand;
}
