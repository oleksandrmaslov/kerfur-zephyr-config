#ifndef KERFUR_MICRO_REACTION_H_
#define KERFUR_MICRO_REACTION_H_

#include <stdbool.h>
#include <stdint.h>

enum micro_reaction_type {
	REACTION_NONE = 0,
	REACTION_BLINK,
	REACTION_GLANCE_LEFT,
	REACTION_GLANCE_RIGHT,
	REACTION_WAKE_BLINK,
	REACTION_STARTLE,
	REACTION_PET_BOW,
	REACTION_HAPPY_BOUNCE,
	REACTION_NOTIF_PING,
	REACTION_NOTIF_BURST,
	REACTION_CONNECT_SPARK,
	REACTION_DISCONNECT_SCAN,
	REACTION_CHARGE_PULSE,
	REACTION_LOW_BATT_SAG,
	REACTION_SLEEP_FADE,
	REACTION_LOOK_UP,
	REACTION_LOOK_DOWN,
	REACTION_COUNT
};

void micro_reaction_init(int64_t now_ms);
bool micro_reaction_trigger(enum micro_reaction_type reaction, int64_t now_ms);
void micro_reaction_tick_100ms(int64_t now_ms);
enum micro_reaction_type micro_reaction_get_active(int64_t now_ms);
int64_t micro_reaction_started_at_ms(void);
int32_t micro_reaction_duration_ms(void);
const char *micro_reaction_str(enum micro_reaction_type reaction);

#endif /* KERFUR_MICRO_REACTION_H_ */
