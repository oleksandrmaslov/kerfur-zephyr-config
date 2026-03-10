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

	/* Derived/session values. */
	uint8_t walk_confidence;
	uint8_t notification_burst_level;
	uint32_t step_count_today;
	uint32_t total_steps_since_boot;

	/* Timestamps. */
	int64_t last_pet_timestamp_ms;
	int64_t last_real_interaction_timestamp_ms;
	int64_t last_motion_timestamp_ms;
	int64_t last_walk_timestamp_ms;
	int64_t last_phone_event_timestamp_ms;
	int64_t last_self_wake_timestamp_ms;
	int64_t last_reaction_timestamp_ms;
	int64_t last_rough_event_timestamp_ms;
	int64_t last_display_state_change_ms;
	int64_t last_expression_change_timestamp_ms;
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

	/* Flags. */
	bool ble_connected;
	bool app_session_active;
	bool charging;
	bool battery_low;
	bool battery_critical;
	bool ambient_wake_enabled;
	bool time_valid;
};

#endif /* KERFUR_PET_STATE_H_ */
