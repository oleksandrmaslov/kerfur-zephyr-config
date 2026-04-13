#ifndef KERFUR_IN_HAND_DETECTOR_H_
#define KERFUR_IN_HAND_DETECTOR_H_

#include <stdbool.h>
#include <stdint.h>

enum in_hand_detector_state {
	IN_HAND_DETECTOR_SURFACE_STILL = 0,
	IN_HAND_DETECTOR_MAYBE_PICKED_UP,
	IN_HAND_DETECTOR_IN_HAND,
	IN_HAND_DETECTOR_WALKING,
	IN_HAND_DETECTOR_SHAKE_EVENT,
};

struct in_hand_detector {
	enum in_hand_detector_state state;
	int16_t surface_gravity_x;
	int16_t surface_gravity_y;
	int16_t surface_gravity_z;
	int64_t still_since_ms;
	int64_t last_motion_wake_ms;
	int64_t last_pickup_ms;
	int64_t last_in_hand_ms;
	int64_t last_orientation_change_ms;
	int64_t last_surface_leave_ms;
	int64_t exit_candidate_since_ms;
	bool surface_still_armed;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	bool in_hand;
	bool pickup_candidate_reported;
	bool picked_up_reported;
};

struct in_hand_detector_input {
	int16_t gravity_x;
	int16_t gravity_y;
	int16_t gravity_z;
	uint16_t motion_mg;
	uint16_t smooth_motion_mg;
	uint16_t jerk_mg;
	uint16_t orientation_delta_mg;
	uint16_t orientation_rate_mg;
	uint32_t gyro_sum_mdps;
	uint8_t walking_confidence;
	uint8_t cadence_confidence;
	uint8_t stability_confidence;
	uint8_t chaos_confidence;
	bool walking_active;
	bool rough_motion;
	bool motion_wake;
	int64_t now_ms;
};

struct in_hand_detector_output {
	enum in_hand_detector_state state;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t look_confidence;
	bool pickup_candidate;
	bool picked_up;
	bool in_hand_enter;
	bool in_hand_exit;
	bool picked_up_recently;
	bool in_hand;
};

void in_hand_detector_init(struct in_hand_detector *detector, int64_t now_ms);
void in_hand_detector_process(struct in_hand_detector *detector,
			      const struct in_hand_detector_input *input,
			      struct in_hand_detector_output *output);

#endif /* KERFUR_IN_HAND_DETECTOR_H_ */
