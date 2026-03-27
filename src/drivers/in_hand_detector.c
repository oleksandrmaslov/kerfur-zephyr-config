#include <string.h>

#include <zephyr/sys/util.h>

#include "drivers/in_hand_detector.h"

#define PICKUP_CANDIDATE_THRESHOLD 50U
#define PICKED_UP_THRESHOLD 72U
#define IN_HAND_ENTER_THRESHOLD 70U
#define IN_HAND_EXIT_THRESHOLD 45U
#define SURFACE_STILL_MIN_MS 2200LL
#define PICKUP_RECENT_MS 6000LL

static uint8_t clamp_u8_0_100(int value)
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

static uint16_t gravity_delta(const struct in_hand_detector *detector,
			      const struct in_hand_detector_input *input)
{
	return (uint16_t)(abs_i16(input->gravity_x - detector->surface_gravity_x) +
			 abs_i16(input->gravity_y - detector->surface_gravity_y) +
			 abs_i16(input->gravity_z - detector->surface_gravity_z));
}

static bool is_surface_still(const struct in_hand_detector_input *input)
{
	return !input->walking_active && !input->rough_motion &&
	       (input->walking_confidence < 35U) &&
	       (input->cadence_confidence < 25U) &&
	       (input->chaos_confidence < 25U) &&
	       (input->motion_mg <= 90U) &&
	       (input->smooth_motion_mg <= 70U) &&
	       (input->orientation_rate_mg <= 42U);
}

void in_hand_detector_init(struct in_hand_detector *detector, int64_t now_ms)
{
	if (detector == NULL) {
		return;
	}

	memset(detector, 0, sizeof(*detector));
	detector->state = IN_HAND_DETECTOR_SURFACE_STILL;
	detector->still_since_ms = now_ms;
}

void in_hand_detector_process(struct in_hand_detector *detector,
			      const struct in_hand_detector_input *input,
			      struct in_hand_detector_output *output)
{
	uint16_t orientation_delta;
	bool surface_still;
	bool had_surface_still;
	bool orientation_medium;
	bool orientation_large;
	bool motion_recent;
	bool smooth_reorientation;
	bool micro_motion;
	bool hand_like_motion;
	int pickup_delta = 0;
	int in_hand_delta = 0;

	if ((detector == NULL) || (input == NULL) || (output == NULL)) {
		return;
	}

	memset(output, 0, sizeof(*output));

	if ((detector->surface_gravity_x == 0) && (detector->surface_gravity_y == 0) &&
	    (detector->surface_gravity_z == 0)) {
		detector->surface_gravity_x = input->gravity_x;
		detector->surface_gravity_y = input->gravity_y;
		detector->surface_gravity_z = input->gravity_z;
	}

	if (input->motion_wake) {
		detector->last_motion_wake_ms = input->now_ms;
	}

	surface_still = is_surface_still(input);
	if (surface_still) {
		if ((detector->still_since_ms == 0LL) ||
		    (detector->state != IN_HAND_DETECTOR_SURFACE_STILL)) {
			detector->still_since_ms = input->now_ms;
		}
		detector->surface_gravity_x =
			(int16_t)((detector->surface_gravity_x * 7 + input->gravity_x) / 8);
		detector->surface_gravity_y =
			(int16_t)((detector->surface_gravity_y * 7 + input->gravity_y) / 8);
		detector->surface_gravity_z =
			(int16_t)((detector->surface_gravity_z * 7 + input->gravity_z) / 8);
	} else {
		detector->last_surface_leave_ms = input->now_ms;
	}

	had_surface_still = (detector->still_since_ms > 0LL) &&
			    ((input->now_ms - detector->still_since_ms) >= SURFACE_STILL_MIN_MS);
	orientation_delta = MAX(input->orientation_delta_mg, gravity_delta(detector, input));
	orientation_medium = orientation_delta >= 105U;
	orientation_large = orientation_delta >= 210U;
	motion_recent = (input->motion_wake ||
			 ((input->now_ms - detector->last_motion_wake_ms) <= 1200LL));
	smooth_reorientation = orientation_medium &&
		(input->orientation_rate_mg >= 24U) &&
		(input->orientation_rate_mg <= 240U) &&
		(input->chaos_confidence <= 65U);
	micro_motion = !input->walking_active && !input->rough_motion &&
		(input->motion_mg >= 25U) && (input->motion_mg <= 260U) &&
		(input->jerk_mg <= 220U) &&
		(input->gyro_sum_mdps <= 28000U) &&
		(input->stability_confidence >= 35U);
	hand_like_motion = smooth_reorientation &&
		(micro_motion || (input->stability_confidence >= 55U)) &&
		(input->walking_confidence < 55U) &&
		(input->cadence_confidence < 55U) &&
		(input->chaos_confidence < 60U);

	if (orientation_medium) {
		detector->last_orientation_change_ms = input->now_ms;
	}

	if (input->rough_motion || (input->chaos_confidence >= 85U)) {
		detector->state = IN_HAND_DETECTOR_SHAKE_EVENT;
		pickup_delta -= 20;
		in_hand_delta -= 26;
	} else if (input->walking_active || (input->walking_confidence >= 70U) ||
		   (input->cadence_confidence >= 75U)) {
		detector->state = IN_HAND_DETECTOR_WALKING;
		pickup_delta -= 8;
		in_hand_delta -= 18;
	} else if (surface_still && had_surface_still) {
		detector->state = IN_HAND_DETECTOR_SURFACE_STILL;
		pickup_delta -= 10;
		in_hand_delta -= 14;
	} else if (detector->in_hand) {
		detector->state = IN_HAND_DETECTOR_IN_HAND;
	} else if ((detector->pickup_confidence > 20U) || hand_like_motion) {
		detector->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	}

	if (had_surface_still && motion_recent && smooth_reorientation &&
	    !input->walking_active && !input->rough_motion) {
		pickup_delta += 8;
		pickup_delta += MIN(12, (int)(orientation_delta / 28U));
		pickup_delta += input->stability_confidence / 20U;
		if (micro_motion) {
			pickup_delta += 4;
		}
		if (orientation_large) {
			pickup_delta += 4;
		}
		pickup_delta -= input->chaos_confidence / 18U;
		detector->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	} else if (!detector->in_hand && !surface_still) {
		pickup_delta -= 2;
	}

	if (hand_like_motion) {
		in_hand_delta += 4;
		in_hand_delta += input->stability_confidence / 18U;
		if (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
			in_hand_delta += 5;
		}
		if (orientation_large) {
			in_hand_delta += 4;
		}
		if (had_surface_still) {
			in_hand_delta += 3;
		}
		in_hand_delta -= input->chaos_confidence / 20U;
	} else if (detector->in_hand && micro_motion &&
		   (input->chaos_confidence < 45U) &&
		   (input->cadence_confidence < 45U)) {
		in_hand_delta += 2;
	} else if (surface_still) {
		in_hand_delta -= 12;
	} else {
		in_hand_delta -= 3;
	}

	if ((input->gyro_sum_mdps >= 32000U) || (input->jerk_mg >= 280U)) {
		in_hand_delta -= 4;
	}

	detector->pickup_confidence =
		clamp_u8_0_100((int)detector->pickup_confidence + pickup_delta);
	detector->in_hand_confidence =
		clamp_u8_0_100((int)detector->in_hand_confidence + in_hand_delta);

	if (!detector->pickup_candidate_reported &&
	    (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD)) {
		output->pickup_candidate = true;
		detector->pickup_candidate_reported = true;
	}

	if (!detector->picked_up_reported &&
	    (detector->pickup_confidence >= PICKED_UP_THRESHOLD) &&
	    (detector->in_hand_confidence >= 45U) && hand_like_motion) {
		output->picked_up = true;
		detector->picked_up_reported = true;
		detector->last_pickup_ms = input->now_ms;
	}

	if (!detector->in_hand && (detector->in_hand_confidence >= IN_HAND_ENTER_THRESHOLD)) {
		detector->in_hand = true;
		detector->state = IN_HAND_DETECTOR_IN_HAND;
		detector->last_in_hand_ms = input->now_ms;
		output->in_hand_enter = true;
	} else if (detector->in_hand &&
		   ((detector->in_hand_confidence <= IN_HAND_EXIT_THRESHOLD) ||
		    input->rough_motion || input->walking_active ||
		    (surface_still && had_surface_still))) {
		detector->in_hand = false;
		output->in_hand_exit = true;
		if (surface_still && had_surface_still) {
			detector->state = IN_HAND_DETECTOR_SURFACE_STILL;
		}
	}

	if (!detector->in_hand && surface_still && had_surface_still &&
	    (detector->pickup_confidence <= 12U) && (detector->in_hand_confidence <= 12U)) {
		detector->pickup_candidate_reported = false;
		detector->picked_up_reported = false;
	}

	output->look_confidence = 0U;
	if (detector->in_hand && !input->walking_active && !input->rough_motion) {
		int look_confidence = detector->in_hand_confidence;

		look_confidence += input->stability_confidence / 3U;
		look_confidence -= input->chaos_confidence / 2U;
		look_confidence -= input->cadence_confidence / 3U;
		if (orientation_medium) {
			look_confidence += 6;
		}
		if (input->motion_mg > 320U) {
			look_confidence -= (input->motion_mg - 320U) / 5U;
		}
		if (input->gyro_sum_mdps > 24000U) {
			look_confidence -= (int)((input->gyro_sum_mdps - 24000U) / 1800U);
		}

		output->look_confidence = clamp_u8_0_100(look_confidence);
	} else if ((detector->state == IN_HAND_DETECTOR_MAYBE_PICKED_UP) &&
		   (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD)) {
		output->look_confidence = clamp_u8_0_100(
			(int)detector->pickup_confidence / 2 +
			(int)input->stability_confidence / 4 -
			(int)input->chaos_confidence / 5);
	}

	output->state = detector->state;
	output->pickup_confidence = detector->pickup_confidence;
	output->in_hand_confidence = detector->in_hand_confidence;
	output->picked_up_recently =
		(detector->last_pickup_ms > 0LL) &&
		((input->now_ms - detector->last_pickup_ms) <= PICKUP_RECENT_MS);
	output->in_hand = detector->in_hand;
}
