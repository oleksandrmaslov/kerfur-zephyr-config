#include <string.h>

#include <zephyr/kernel.h>

#include "behavior/behavior_engine.h"

static int16_t clamp_0_100(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 100) {
		return 100;
	}
	return (int16_t)value;
}

static void clamp_state(struct pet_state *state)
{
	state->energy = clamp_0_100(state->energy);
	state->sleepiness = clamp_0_100(state->sleepiness);
	state->attachment = clamp_0_100(state->attachment);
	state->boredom = clamp_0_100(state->boredom);
	state->stress = clamp_0_100(state->stress);
	state->arousal = clamp_0_100(state->arousal);
	state->social_load = clamp_0_100(state->social_load);
}

static void mark_real_interaction(struct pet_state *state, int64_t now_ms)
{
	state->last_interaction_timestamp_ms = now_ms;
}

static void update_mode_transitions(struct pet_state *state)
{
	if (state->current_mode == PET_MODE_ASLEEP) {
		if ((state->arousal > 70) || (state->sleepiness < 25)) {
			state->current_mode = PET_MODE_AWAKE;
		}
		return;
	}

	if ((state->energy < 10) && (state->sleepiness > 85)) {
		state->current_mode = PET_MODE_ASLEEP;
		return;
	}

	if ((state->energy < 20) && (state->sleepiness > 70)) {
		state->current_mode = PET_MODE_DROWSY;
		return;
	}

	if ((state->energy > 25) && (state->sleepiness < 65)) {
		state->current_mode = PET_MODE_AWAKE;
	}
}

static enum pet_expression compute_expression(const struct pet_state *state, int64_t now_ms)
{
	int64_t idle_s = (now_ms - state->last_interaction_timestamp_ms) / MSEC_PER_SEC;
	int idle_bonus = (idle_s > 90) ? 30 : (int)(idle_s / 3);
	int score_idle = 20 + (state->energy / 5) - (state->arousal / 6);
	int score_happy = state->attachment + (state->energy / 2) - state->stress - (state->sleepiness / 2);
	int score_sleepy = state->sleepiness + ((100 - state->energy) / 2) + (state->battery_low ? 20 : 0);
	int score_curious = state->arousal + (state->social_load / 2) + ((100 - state->boredom) / 3) - (state->sleepiness / 3);
	int score_needy = state->boredom + ((100 - state->attachment) / 2) + idle_bonus;
	int score_stressed = state->stress + (state->arousal / 2) + (state->battery_low ? 10 : 0);

	enum pet_expression best = PET_EXPR_IDLE;
	int best_score = score_idle;

	if (state->current_mode == PET_MODE_ASLEEP) {
		return PET_EXPR_ASLEEP;
	}

	if (state->current_mode == PET_MODE_DROWSY) {
		score_sleepy += 10;
	}

	if (score_happy > best_score) {
		best = PET_EXPR_HAPPY;
		best_score = score_happy;
	}
	if (score_sleepy > best_score) {
		best = PET_EXPR_SLEEPY;
		best_score = score_sleepy;
	}
	if (score_curious > best_score) {
		best = PET_EXPR_CURIOUS;
		best_score = score_curious;
	}
	if (score_needy > best_score) {
		best = PET_EXPR_NEEDY;
		best_score = score_needy;
	}
	if (score_stressed > best_score) {
		best = PET_EXPR_STRESSED;
	}

	return best;
}

static void apply_tick_update(struct pet_state *state, int64_t now_ms)
{
	int64_t idle_s = (now_ms - state->last_interaction_timestamp_ms) / MSEC_PER_SEC;

	state->uptime_seconds++;
	state->social_load -= 1;
	state->arousal -= 1;

	if (state->current_mode == PET_MODE_ASLEEP) {
		state->energy += 2;
		state->sleepiness -= 3;
		state->stress -= 2;
		state->boredom += 1;
	} else {
		state->energy -= state->battery_low ? 2 : 1;
		state->sleepiness += 2;
		state->stress += (state->arousal > 60) ? 1 : -1;
		state->boredom += (idle_s > 10) ? 2 : 1;
		if (idle_s > 40) {
			state->attachment -= 1;
		}
	}

	clamp_state(state);
}

void behavior_engine_init(struct pet_state *state, int64_t now_ms)
{
	memset(state, 0, sizeof(*state));

	state->energy = 70;
	state->sleepiness = 20;
	state->attachment = 50;
	state->boredom = 25;
	state->stress = 20;
	state->arousal = 35;
	state->social_load = 10;
	state->last_interaction_timestamp_ms = now_ms;
	state->current_mode = PET_MODE_AWAKE;
	state->expression = PET_EXPR_IDLE;
}

void behavior_engine_handle_event(struct pet_state *state, const struct app_event *event)
{
	switch (event->type) {
	case APP_EVENT_TICK_1S:
		apply_tick_update(state, event->timestamp_ms);
		break;
	case APP_EVENT_USER_BUTTON_PRESS:
		state->arousal += 3;
		break;
	case APP_EVENT_MOCK_PET:
		state->attachment += 8;
		state->boredom -= 8;
		state->stress -= 6;
		state->arousal += 4;
		state->energy += 1;
		mark_real_interaction(state, event->timestamp_ms);
		break;
	case APP_EVENT_MOCK_SHAKE:
		state->arousal += 18;
		state->stress += 10;
		state->sleepiness -= 6;
		mark_real_interaction(state, event->timestamp_ms);
		break;
	case APP_EVENT_MOCK_NOTIFICATION:
		state->social_load += 15;
		state->arousal += 8;
		state->boredom -= 4;
		break;
	case APP_EVENT_IDLE_TIMEOUT:
		if (state->current_mode == PET_MODE_AWAKE) {
			state->current_mode = PET_MODE_DROWSY;
		}
		break;
	case APP_EVENT_WAKE:
		state->current_mode = PET_MODE_AWAKE;
		state->sleepiness -= 8;
		state->arousal += 5;
		break;
	case APP_EVENT_SLEEP_REQUEST:
		state->current_mode = PET_MODE_ASLEEP;
		state->arousal -= 6;
		break;
	case APP_EVENT_BATTERY_LOW:
		state->battery_low = true;
		state->stress += 6;
		state->sleepiness += 6;
		break;
	case APP_EVENT_BLE_CONNECTED:
		state->ble_connected = true;
		state->social_load += 5;
		break;
	case APP_EVENT_BLE_DISCONNECTED:
		state->ble_connected = false;
		break;
	default:
		break;
	}

	clamp_state(state);
	update_mode_transitions(state);
	state->expression = compute_expression(state, event->timestamp_ms);
}

const char *pet_mode_str(enum pet_mode mode)
{
	switch (mode) {
	case PET_MODE_AWAKE:
		return "AWAKE";
	case PET_MODE_DROWSY:
		return "DROWSY";
	case PET_MODE_ASLEEP:
		return "ASLEEP";
	default:
		return "UNKNOWN";
	}
}

const char *pet_expression_str(enum pet_expression expression)
{
	switch (expression) {
	case PET_EXPR_IDLE:
		return "IDLE";
	case PET_EXPR_HAPPY:
		return "HAPPY";
	case PET_EXPR_SLEEPY:
		return "SLEEPY";
	case PET_EXPR_CURIOUS:
		return "CURIOUS";
	case PET_EXPR_NEEDY:
		return "NEEDY";
	case PET_EXPR_STRESSED:
		return "STRESSED";
	case PET_EXPR_ASLEEP:
		return "ASLEEP";
	default:
		return "UNKNOWN";
	}
}
