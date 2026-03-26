#include <string.h>

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
	bool micro_motion;
	bool hand_like_motion;

	if ((detector == NULL) || (input == NULL) || (output == NULL)) {
		return;
	}

	memset(output, 0, sizeof(*output));

	if (detector->surface_gravity_x == 0 && detector->surface_gravity_y == 0 &&
	    detector->surface_gravity_z == 0) {
		detector->surface_gravity_x = input->gravity_x;
		detector->surface_gravity_y = input->gravity_y;
		detector->surface_gravity_z = input->gravity_z;
	}

	if (input->motion_wake) {
		detector->last_motion_wake_ms = input->now_ms;
	}

	surface_still = !input->walking_active && !input->rough_motion &&
		       (input->motion_mg <= 90U) && (input->smooth_motion_mg <= 55U);
	if (surface_still) {
		if ((detector->still_since_ms == 0LL) ||
		    (detector->state != IN_HAND_DETECTOR_SURFACE_STILL)) {
			detector->still_since_ms = input->now_ms;
		}
		detector->surface_gravity_x =
			(int16_t)((detector->surface_gravity_x * 3 + input->gravity_x) / 4);
		detector->surface_gravity_y =
			(int16_t)((detector->surface_gravity_y * 3 + input->gravity_y) / 4);
		detector->surface_gravity_z =
			(int16_t)((detector->surface_gravity_z * 3 + input->gravity_z) / 4);
	}

	had_surface_still = (detector->still_since_ms > 0LL) &&
			    ((input->now_ms - detector->still_since_ms) >= SURFACE_STILL_MIN_MS);
	orientation_delta = gravity_delta(detector, input);
	orientation_medium = orientation_delta >= 110U;
	orientation_large = orientation_delta >= 220U;
	micro_motion = !input->walking_active && !input->rough_motion &&
		      (input->motion_mg >= 35U) && (input->motion_mg <= 280U) &&
		      (input->smooth_motion_mg >= 15U) && (input->smooth_motion_mg <= 240U);
	hand_like_motion = (orientation_medium || micro_motion) &&
			  !input->walking_active && !input->rough_motion &&
			  (input->walking_confidence < 55U);

	if (orientation_medium) {
		detector->last_orientation_change_ms = input->now_ms;
	}

	if (input->rough_motion) {
		detector->state = IN_HAND_DETECTOR_SHAKE_EVENT;
		detector->pickup_confidence = clamp_u8_0_100((int)detector->pickup_confidence - 18);
		detector->in_hand_confidence = clamp_u8_0_100((int)detector->in_hand_confidence - 24);
	} else if (input->walking_active || (input->walking_confidence >= 70U)) {
		detector->state = IN_HAND_DETECTOR_WALKING;
		detector->pickup_confidence = clamp_u8_0_100((int)detector->pickup_confidence - 6);
		detector->in_hand_confidence = clamp_u8_0_100((int)detector->in_hand_confidence - 14);
	} else if (surface_still && had_surface_still) {
		detector->state = IN_HAND_DETECTOR_SURFACE_STILL;
		detector->pickup_confidence = clamp_u8_0_100((int)detector->pickup_confidence - 10);
		detector->in_hand_confidence = clamp_u8_0_100((int)detector->in_hand_confidence - 14);
	} else if (detector->in_hand) {
		detector->state = IN_HAND_DETECTOR_IN_HAND;
	} else if ((detector->pickup_confidence > 20U) || hand_like_motion) {
		detector->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	}

	if (had_surface_still &&
	    (input->motion_wake || (input->motion_mg >= 150U) || (input->smooth_motion_mg >= 80U)) &&
	    orientation_medium && !input->walking_active && !input->rough_motion) {
		int pickup_gain = orientation_large ? 14 : 10;

		if (micro_motion) {
			pickup_gain += 6;
		}

		detector->pickup_confidence =
			clamp_u8_0_100((int)detector->pickup_confidence + pickup_gain);
		detector->state = IN_HAND_DETECTOR_MAYBE_PICKED_UP;
	} else if (!detector->in_hand && !surface_still) {
		detector->pickup_confidence =
			clamp_u8_0_100((int)detector->pickup_confidence - 2);
	}

	if (!detector->pickup_candidate_reported &&
	    (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD)) {
		output->pickup_candidate = true;
		detector->pickup_candidate_reported = true;
	}

	if (hand_like_motion) {
		int in_hand_gain = 6;

		if (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD) {
			in_hand_gain += 5;
		}
		if (orientation_large) {
			in_hand_gain += 4;
		}

		detector->in_hand_confidence =
			clamp_u8_0_100((int)detector->in_hand_confidence + in_hand_gain);
	} else if (surface_still) {
		detector->in_hand_confidence =
			clamp_u8_0_100((int)detector->in_hand_confidence - 12);
	} else {
		detector->in_hand_confidence =
			clamp_u8_0_100((int)detector->in_hand_confidence - 3);
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
		   (detector->in_hand_confidence <= IN_HAND_EXIT_THRESHOLD || input->rough_motion ||
		    input->walking_active || (surface_still && had_surface_still))) {
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

		if (orientation_medium) {
			look_confidence += 10;
		}
		if (input->motion_mg > 280U) {
			look_confidence -= (input->motion_mg - 280U) / 4U;
		}
		if (input->smooth_motion_mg > 260U) {
			look_confidence -= (input->smooth_motion_mg - 260U) / 3U;
		}

		output->look_confidence = clamp_u8_0_100(look_confidence);
	} else if ((detector->state == IN_HAND_DETECTOR_MAYBE_PICKED_UP) &&
		   (detector->pickup_confidence >= PICKUP_CANDIDATE_THRESHOLD)) {
		output->look_confidence = detector->pickup_confidence / 2U;
	}

	output->state = detector->state;
	output->pickup_confidence = detector->pickup_confidence;
	output->in_hand_confidence = detector->in_hand_confidence;
	output->picked_up_recently =
		(detector->last_pickup_ms > 0LL) &&
		((input->now_ms - detector->last_pickup_ms) <= PICKUP_RECENT_MS);
	output->in_hand = detector->in_hand;
}
