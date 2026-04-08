#include <string.h>

#include <zephyr/sys/util.h>

#include "drivers/in_hand_detector.h"

/* Confidence thresholds. */
#define PICKUP_CANDIDATE_THRESHOLD 35U
#define PICKED_UP_THRESHOLD        40U
#define IN_HAND_ENTER_THRESHOLD    70U
#define IN_HAND_EXIT_THRESHOLD     60U

/* Timing. */
#define IN_HAND_EXIT_CONFIRM_MS        600LL
#define IN_HAND_EXIT_HOLD_AFTER_ENTER  800LL
#define SURFACE_STILL_MIN_MS          1500LL
#define SURFACE_STILL_RECENT_MS       2500LL
#define PICKUP_RECENT_MS              6000LL

static uint8_t clamp_u8(int value)
{
	if (value < 0) {
		return 0U;
	}
	if (value > 100) {
		return 100U;
	}
	return (uint8_t)value;
}

static int abs_i16(int16_t value)
{
	return (value < 0) ? -value : value;
}

static uint16_t gravity_distance(const struct in_hand_detector *det,
				 const struct in_hand_detector_input *in)
{
	return (uint16_t)(abs_i16(in->gravity_x - det->surface_gravity_x) +
			  abs_i16(in->gravity_y - det->surface_gravity_y) +
			  abs_i16(in->gravity_z - det->surface_gravity_z));
}

/* ── Surface-still classifier ─────────────────────────────────────────── */

static bool is_surface_still(const struct in_hand_detector_input *in)
{
	return !in->walking_active &&
	       !in->rough_motion &&
	       in->walking_confidence < 30U &&
	       in->chaos_confidence   < 20U &&
	       in->motion_mg          <= 100U &&
	       in->smooth_motion_mg   <= 80U &&
	       in->orientation_rate_mg <= 50U;
}

/* Looser stillness window: low motion and no orientation drift, even if a
 * stray vibration spike crosses the strict surface-still gate. Used to bias
 * the in-hand detector toward exit when the device is clearly resting. */
static bool is_near_still(const struct in_hand_detector_input *in)
{
	return !in->walking_active &&
	       !in->rough_motion &&
	       in->motion_mg          <= 200U &&
	       in->smooth_motion_mg   <= 150U &&
	       in->orientation_rate_mg <= 80U &&
	       in->orientation_delta_mg <= 80U &&
	       in->chaos_confidence    < 50U;
}

/* ── Hand-like motion classifier ──────────────────────────────────────── */

static bool is_smooth_reorientation(const struct in_hand_detector_input *in)
{
	/* Slow, steady gravity vector change — characteristic of being held. */
	return in->orientation_delta_mg >= 8U &&
	       in->orientation_delta_mg <= 300U &&
	       in->orientation_rate_mg  >= 6U &&
	       in->orientation_rate_mg  <= 300U &&
	       in->chaos_confidence     <= 60U;
}

static bool is_micro_motion(const struct in_hand_detector_input *in)
{
	/* Small, non-periodic body tremor. */
	return in->motion_mg >= 12U &&
	       in->motion_mg <= 300U &&
	       in->jerk_mg   <= 280U &&
	       in->gyro_sum_mdps <= 30000U;
}

static bool is_hand_like(const struct in_hand_detector_input *in)
{
	if (in->rough_motion || in->walking_active) {
		return false;
	}
	if (in->walking_confidence >= 60U || in->cadence_confidence >= 60U) {
		return false;
	}
	if (in->chaos_confidence >= 65U) {
		return false;
	}
	/* Hand-held motion is small but persistent and almost always involves
	 * gravity vector reorientation. Pure linear noise from a vibrating
	 * surface (passing footsteps, fan, AC) routinely produces 30-60mg
	 * spikes that must NOT count, otherwise the detector pins itself in
	 * the in-hand state forever. Require either real reorientation, or
	 * sustained inter-sample motion above the noise floor. */
	const bool orientation_evidence =
		in->orientation_delta_mg >= 25U || in->orientation_rate_mg >= 25U;
	const bool sustained_motion =
		in->smooth_motion_mg >= 40U && in->motion_mg >= 60U;
	return orientation_evidence || sustained_motion;
}

/* ── Public API ───────────────────────────────────────────────────────── */

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
	bool surface_confirmed;
	bool had_surface_recently;
	bool hand_like;
	bool orientation_moved;
	int pickup_delta = 0;
	int in_hand_delta = 0;

	if (det == NULL || in == NULL || out == NULL) {
		return;
	}

	memset(out, 0, sizeof(*out));

	/* ── Surface-still tracking ────────────────────────────────────── */

	surface_still = is_surface_still(in);

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

	surface_confirmed = det->surface_still_armed &&
			    ((in->now_ms - det->still_since_ms) >= SURFACE_STILL_MIN_MS);

	had_surface_recently = (det->last_surface_leave_ms > 0LL) &&
			       ((in->now_ms - det->last_surface_leave_ms) <= SURFACE_STILL_RECENT_MS);

	/* ── Motion classifiers ────────────────────────────────────────── */

	hand_like = is_hand_like(in);
	orientation_moved = gravity_distance(det, in) >= 120U;

	/* ── Pickup confidence ─────────────────────────────────────────── */

	if (in->rough_motion) {
		pickup_delta = -15;
	} else if (in->walking_active || in->cadence_confidence >= 60U) {
		pickup_delta = -8;
	} else if (surface_still) {
		pickup_delta = -5;
	} else if (hand_like && (had_surface_recently || in->motion_wake)) {
		pickup_delta = 6;
		if (is_smooth_reorientation(in)) {
			pickup_delta += 4;
		}
		if (orientation_moved) {
			pickup_delta += 5;
		}
		if (is_micro_motion(in)) {
			pickup_delta += 3;
		}
		pickup_delta += (int)in->stability_confidence / 15;
		pickup_delta -= (int)in->chaos_confidence / 15;
	} else if (!det->in_hand) {
		pickup_delta = -2;
	}

	/* ── In-hand confidence ────────────────────────────────────────── */

	if (in->rough_motion) {
		in_hand_delta = -8;
	} else if (hand_like) {
		in_hand_delta = 8;
		if (det->pickup_confidence >= 30U) {
			in_hand_delta += 4;
		}
		if (orientation_moved) {
			in_hand_delta += 3;
		}
		if (had_surface_recently) {
			in_hand_delta += 3;
		}
		in_hand_delta += (int)in->stability_confidence / 15;
		in_hand_delta -= (int)in->chaos_confidence / 20;
	} else if (det->in_hand) {
		/* No hand-like signal this tick but still flagged in_hand.
		 * Gravity drift from the saved surface reference is the cleanest
		 * way to tell a gentle hold (drift > 0) from a vibrating surface
		 * (drift == 0) — pin confidence in the former, decay in the latter. */
		if (surface_still) {
			in_hand_delta = -8;
		} else if (orientation_moved) {
			in_hand_delta = 0;
		} else if (is_near_still(in)) {
			in_hand_delta = -4;
		} else {
			in_hand_delta = -1;
		}
	} else if (surface_still) {
		in_hand_delta = -3;
	} else {
		in_hand_delta = -1;
	}

	det->pickup_confidence   = clamp_u8((int)det->pickup_confidence + pickup_delta);
	det->in_hand_confidence  = clamp_u8((int)det->in_hand_confidence + in_hand_delta);

	/* ── One-shot events ───────────────────────────────────────────── */

	/* Reset one-shot flags when confidence has clearly dropped.
	 * Use generous thresholds so quick re-pickup is detected. */
	if (!det->in_hand) {
		if (det->pickup_candidate_reported &&
		    det->pickup_confidence < (PICKUP_CANDIDATE_THRESHOLD - 8U)) {
			det->pickup_candidate_reported = false;
		}
		if (det->picked_up_reported &&
		    (det->pickup_confidence <= 25U ||
		     (surface_confirmed && det->in_hand_confidence <= 20U))) {
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
	    det->in_hand_confidence >= 35U &&
	    hand_like) {
		out->picked_up = true;
		det->picked_up_reported = true;
		det->last_pickup_ms = in->now_ms;
	}

	/* ── In-hand state machine ─────────────────────────────────────── */

	if (!det->in_hand && det->in_hand_confidence >= IN_HAND_ENTER_THRESHOLD) {
		det->in_hand = true;
		det->state = IN_HAND_DETECTOR_IN_HAND;
		det->last_in_hand_ms = in->now_ms;
		det->exit_candidate_since_ms = 0LL;
		out->in_hand_enter = true;
	} else if (det->in_hand) {
		bool immediate_exit = in->rough_motion;
		bool soft_exit = det->in_hand_confidence <= IN_HAND_EXIT_THRESHOLD ||
				 in->walking_active || surface_confirmed;
		bool exit_allowed =
			(in->now_ms - det->last_in_hand_ms) >= IN_HAND_EXIT_HOLD_AFTER_ENTER;

		if (immediate_exit) {
			det->in_hand = false;
			det->exit_candidate_since_ms = 0LL;
			out->in_hand_exit = true;
		} else if (soft_exit) {
			if (det->exit_candidate_since_ms == 0LL) {
				det->exit_candidate_since_ms = in->now_ms;
			}
			if (exit_allowed &&
			    (in->now_ms - det->exit_candidate_since_ms) >= IN_HAND_EXIT_CONFIRM_MS) {
				det->in_hand = false;
				det->exit_candidate_since_ms = 0LL;
				out->in_hand_exit = true;
				if (surface_confirmed) {
					det->state = IN_HAND_DETECTOR_SURFACE_STILL;
				}
			}
		} else {
			det->exit_candidate_since_ms = 0LL;
		}
	}

	/* Update state machine label. */
	if (det->in_hand) {
		det->state = in->walking_active ? IN_HAND_DETECTOR_WALKING :
						  IN_HAND_DETECTOR_IN_HAND;
	} else if (in->rough_motion) {
		det->state = IN_HAND_DETECTOR_SHAKE_EVENT;
	} else if (det->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
		det->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	} else if (surface_confirmed) {
		det->state = IN_HAND_DETECTOR_SURFACE_STILL;
	}

	/* ── Look confidence ───────────────────────────────────────────── */

	out->look_confidence = 0U;

	if (det->in_hand && !in->walking_active && !in->rough_motion) {
		int look = (int)det->in_hand_confidence;

		look += (int)in->stability_confidence / 3;
		look -= (int)in->chaos_confidence / 3;
		look -= (int)in->cadence_confidence / 4;
		if (in->orientation_delta_mg >= 8U && in->orientation_delta_mg <= 200U) {
			look += 8;
		}
		if (in->motion_mg > 350U) {
			look -= (int)(in->motion_mg - 350U) / 6;
		}
		if (in->gyro_sum_mdps > 25000U) {
			look -= (int)(in->gyro_sum_mdps - 25000U) / 2000;
		}
		out->look_confidence = clamp_u8(look);
	} else if (det->state == IN_HAND_DETECTOR_MAYBE_PICKED_UP &&
		   det->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
		out->look_confidence = clamp_u8(
			(int)det->pickup_confidence / 2 +
			(int)in->stability_confidence / 4 -
			(int)in->chaos_confidence / 5);
	}

	/* ── Output ────────────────────────────────────────────────────── */

	out->state = det->state;
	out->pickup_confidence = det->pickup_confidence;
	out->in_hand_confidence = det->in_hand_confidence;
	out->picked_up_recently =
		(det->last_pickup_ms > 0LL) &&
		((in->now_ms - det->last_pickup_ms) <= PICKUP_RECENT_MS);
	out->in_hand = det->in_hand;
}
