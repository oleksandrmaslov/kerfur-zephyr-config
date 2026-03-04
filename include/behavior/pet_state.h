#ifndef KERFUR_PET_STATE_H_
#define KERFUR_PET_STATE_H_

#include <stdbool.h>
#include <stdint.h>

enum pet_mode {
	PET_MODE_AWAKE = 0,
	PET_MODE_DROWSY,
	PET_MODE_ASLEEP
};

enum pet_expression {
	PET_EXPR_IDLE = 0,
	PET_EXPR_HAPPY,
	PET_EXPR_SLEEPY,
	PET_EXPR_CURIOUS,
	PET_EXPR_NEEDY,
	PET_EXPR_STRESSED,
	PET_EXPR_ASLEEP
};

struct pet_state {
	int16_t energy;
	int16_t sleepiness;
	int16_t attachment;
	int16_t boredom;
	int16_t stress;
	int16_t arousal;
	int16_t social_load;

	int64_t last_interaction_timestamp_ms;
	int64_t last_phone_notification_timestamp_ms;
	uint32_t uptime_seconds;

	enum pet_mode current_mode;
	enum pet_expression expression;

	bool ble_connected;
	bool battery_low;
};

#endif /* KERFUR_PET_STATE_H_ */
