#include <string.h>

#include <zephyr/kernel.h>

#include "behavior/micro_reaction.h"

struct micro_reaction_runtime {
	enum micro_reaction_type active;
	int64_t started_at_ms;
	int32_t duration_ms;
	int64_t cooldown_until_ms[REACTION_COUNT];
};

static struct micro_reaction_runtime g_reaction;

static int32_t reaction_duration_ms(enum micro_reaction_type reaction)
{
	switch (reaction) {
	case REACTION_BLINK:
		return 220;
	case REACTION_GLANCE_LEFT:
	case REACTION_GLANCE_RIGHT:
		return 800;
	case REACTION_WAKE_BLINK:
		return 1000;
	case REACTION_STARTLE:
		return 1200;
	case REACTION_PET_BOW:
		return 1400;
	case REACTION_HAPPY_BOUNCE:
		return 1300;
	case REACTION_NOTIF_PING:
		return 1000;
	case REACTION_NOTIF_BURST:
		return 1700;
	case REACTION_CONNECT_SPARK:
		return 1800;
	case REACTION_DISCONNECT_SCAN:
		return 2000;
	case REACTION_CHARGE_PULSE:
		return 2200;
	case REACTION_LOW_BATT_SAG:
		return 1800;
	case REACTION_SLEEP_FADE:
		return 1000;
	case REACTION_LOOK_UP:
	case REACTION_LOOK_DOWN:
		return 700;
	case REACTION_NONE:
	default:
		return 0;
	}
}

static int32_t reaction_cooldown_ms(enum micro_reaction_type reaction)
{
	switch (reaction) {
	case REACTION_BLINK:
		return 400;
	case REACTION_NOTIF_BURST:
	case REACTION_STARTLE:
		return 2200;
	case REACTION_CHARGE_PULSE:
		return 5000;
	default:
		return 900;
	}
}

void micro_reaction_init(int64_t now_ms)
{
	(void)memset(&g_reaction, 0, sizeof(g_reaction));
	g_reaction.active = REACTION_NONE;
	g_reaction.started_at_ms = now_ms;
}

bool micro_reaction_trigger(enum micro_reaction_type reaction, int64_t now_ms)
{
	int32_t duration_ms;
	int32_t cooldown_ms;

	if ((reaction <= REACTION_NONE) || (reaction >= REACTION_COUNT)) {
		return false;
	}

	if (now_ms < g_reaction.cooldown_until_ms[reaction]) {
		return false;
	}

	duration_ms = reaction_duration_ms(reaction);
	cooldown_ms = reaction_cooldown_ms(reaction);

	g_reaction.active = reaction;
	g_reaction.duration_ms = duration_ms;
	g_reaction.started_at_ms = now_ms;
	g_reaction.cooldown_until_ms[reaction] = now_ms + cooldown_ms;
	return true;
}

void micro_reaction_tick_100ms(int64_t now_ms)
{
	if (g_reaction.active == REACTION_NONE) {
		return;
	}

	if (now_ms >= (g_reaction.started_at_ms + g_reaction.duration_ms)) {
		g_reaction.active = REACTION_NONE;
		g_reaction.duration_ms = 0;
	}
}

enum micro_reaction_type micro_reaction_get_active(int64_t now_ms)
{
	micro_reaction_tick_100ms(now_ms);
	return g_reaction.active;
}

int64_t micro_reaction_started_at_ms(void)
{
	return g_reaction.started_at_ms;
}

int32_t micro_reaction_duration_ms(void)
{
	return g_reaction.duration_ms;
}

const char *micro_reaction_str(enum micro_reaction_type reaction)
{
	switch (reaction) {
	case REACTION_NONE:
		return "NONE";
	case REACTION_BLINK:
		return "BLINK";
	case REACTION_GLANCE_LEFT:
		return "GLANCE_LEFT";
	case REACTION_GLANCE_RIGHT:
		return "GLANCE_RIGHT";
	case REACTION_WAKE_BLINK:
		return "WAKE_BLINK";
	case REACTION_STARTLE:
		return "STARTLE";
	case REACTION_PET_BOW:
		return "PET_BOW";
	case REACTION_HAPPY_BOUNCE:
		return "HAPPY_BOUNCE";
	case REACTION_NOTIF_PING:
		return "NOTIF_PING";
	case REACTION_NOTIF_BURST:
		return "NOTIF_BURST";
	case REACTION_CONNECT_SPARK:
		return "CONNECT_SPARK";
	case REACTION_DISCONNECT_SCAN:
		return "DISCONNECT_SCAN";
	case REACTION_CHARGE_PULSE:
		return "CHARGE_PULSE";
	case REACTION_LOW_BATT_SAG:
		return "LOW_BATT_SAG";
	case REACTION_SLEEP_FADE:
		return "SLEEP_FADE";
	case REACTION_LOOK_UP:
		return "LOOK_UP";
	case REACTION_LOOK_DOWN:
		return "LOOK_DOWN";
	default:
		return "UNKNOWN";
	}
}
