#ifndef KERFUR_PET_STATE_H_
#define KERFUR_PET_STATE_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/micro_reaction.h"

enum pet_mode {
	PET_MODE_ASLEEP = 0,
	PET_MODE_DROWSY,
	PET_MODE_IDLE,
	PET_MODE_INTERACTING,
	PET_MODE_WALK_AWAKE,
	PET_MODE_TASK_ALERT,
	PET_MODE_CHARGING,
	PET_MODE_LOW_POWER,
	PET_MODE_OVERLOADED
};

enum pet_expression {
	PET_EXPR_CALM = 0,
	PET_EXPR_CURIOUS,
	PET_EXPR_CONTENT,
	PET_EXPR_HAPPY,
	PET_EXPR_PLAYFUL,
	PET_EXPR_SLEEPY,
	PET_EXPR_NEEDY,
	PET_EXPR_LONELY,
	PET_EXPR_ANNOYED,
	PET_EXPR_OVERSTIMULATED,
	PET_EXPR_COZY,
	PET_EXPR_DRAINED,
	PET_EXPR_ASLEEP
};

enum pet_display_state {
	DISPLAY_FOREGROUND = 0,
	DISPLAY_AMBIENT,
	DISPLAY_OFF,
};

/* Personality biases drive gains/decays in the behavior engine.
 * IDs are persisted in emotional memory — append only, never reorder. */
enum pet_personality {
	PET_PERSONALITY_BALANCED = 0,
	PET_PERSONALITY_CURIOUS,
	PET_PERSONALITY_SHY,
	PET_PERSONALITY_PLAYFUL,
	PET_PERSONALITY_CALM,
	PET_PERSONALITY_COUNT
};

struct pet_state {
	/* Core internal variables (0..100). */
	int16_t energy;
	int16_t sleepiness;
	int16_t attachment;
	int16_t boredom;
	int16_t stress;
	int16_t arousal;
	int16_t social_load;
	int16_t trust;
	int16_t curiosity;
	/* Slow valence: how life has felt lately. 50 = neutral; moves at most
	 * a couple of points per minute, drifts back to 50 over hours. */
	int16_t mood;

	/* Derived/session values. */
	uint8_t walk_confidence;
	uint8_t walking_confidence;
	uint8_t notification_burst_level;
	uint32_t step_count_today;
	uint32_t total_steps_since_boot;
	uint16_t last_hw_step_counter;
	bool walking_active;
	int64_t walking_session_start_ms;

	/* Timestamps. */
	int64_t last_pet_timestamp_ms;
	int64_t last_real_interaction_timestamp_ms;
	int64_t last_motion_timestamp_ms;
	int64_t last_walk_timestamp_ms;
	int64_t last_pickup_timestamp_ms;
	int64_t last_in_hand_timestamp_ms;
	int64_t last_motion_sample_timestamp_ms;
	int64_t last_still_timestamp_ms;
	int64_t last_phone_event_timestamp_ms;
	int64_t last_self_wake_timestamp_ms;
	int64_t last_reaction_timestamp_ms;
	int64_t last_rough_event_timestamp_ms;
	int64_t last_display_state_change_ms;
	int64_t last_expression_change_timestamp_ms;
	int64_t last_motion_wake_reaction_ms;
	int64_t uptime_seconds;

	/* Time sync cache (no RTC required in this phase). */
	int64_t unix_time_at_sync;
	int64_t uptime_at_sync_ms;
	int16_t tz_offset_minutes;

	/* Current states. */
	enum pet_mode current_mode;
	enum pet_expression current_expression;
	enum micro_reaction_type current_reaction;
	enum pet_display_state current_display_state;
	int16_t current_indicator;
	int16_t current_overlay;
	int16_t look_target_x;
	int16_t look_target_y;
	int16_t look_render_x;
	int16_t look_render_y;
	uint8_t look_confidence;
	bool picked_up_recently;
	bool in_hand;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	int8_t battery_percent;
	bool battery_percent_known;

	/* Flags. */
	bool ble_connected;
	bool app_session_active;
	bool charging;
	bool battery_low;
	bool battery_critical;
	bool ambient_wake_enabled;
	bool time_valid;
	bool dynamic_pupils_forced_disabled;

	/* Social / nearby (Kerfur-to-Kerfur). */
	bool peer_nearby;
	bool peer_known_friend;
	bool social_overload;
	bool encounter_sync_pending;
	uint32_t current_active_peer_id;
	int16_t social_indicator;             /* enum kerfur_face_indicator_id; 0 = none */
	int64_t social_indicator_until_ms;
	int64_t last_social_glance_ms;
};

#endif /* KERFUR_PET_STATE_H_ */
