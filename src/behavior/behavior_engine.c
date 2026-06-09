/*
 * Behavior engine — "mini AI" rewrite.
 *
 * Pipeline (every event):
 *   1. Interpret incoming event in context
 *   2. Apply immediate micro-reaction
 *   3. Update long-lived pet_state variables
 *   4. Update short-lived afterglow context
 *   5. Resolve behavioral intent (throttled)
 *   6. Resolve mode
 *   7. Resolve expression (with hysteresis)
 *
 * Design goals:
 *   - Same event feels different depending on current emotional state
 *   - Short-lived context creates emotional momentum / afterglow
 *   - Intent gives the pet a brief behavioral "leaning"
 *   - Expression scoring integrates context + intent for stability
 *   - Zero new fields in pet_state (all new state is file-local runtime)
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "behavior/behavior_engine.h"
#include "behavior/micro_reaction.h"
#include "ui/generated/kerfur_face_assets.h"

LOG_MODULE_REGISTER(behavior_engine, CONFIG_LOG_DEFAULT_LEVEL);

#define PET_EXPR_COUNT (PET_EXPR_ASLEEP + 1)

/* ── Short-lived context ("afterglow") ───────────────────────────────
 *
 * These decay naturally every second and give the pet the illusion of
 * remembering what just happened.  They live only in file-local runtime,
 * NOT in pet_state.
 *
 *   stimulation  — how much has been going on recently  (decay −3/s)
 *   comfort      — recent soothing / positive touch     (decay −2/s)
 *   social_warmth — recent social / peer presence        (decay −1/s)
 */

struct behavior_context {
	int16_t stimulation;
	int16_t comfort;
	int16_t social_warmth;
};

/* ── Behavioral intent ───────────────────────────────────────────────
 *
 * What the pet "wants" right now.  Resolved every few seconds from
 * current state + context, with hysteresis to prevent jitter.
 */

enum pet_intent {
	PET_INTENT_NONE = 0,
	PET_INTENT_REST,
	PET_INTENT_SEEK_ATTENTION,
	PET_INTENT_OBSERVE,
	PET_INTENT_PLAY,
	PET_INTENT_SELF_SOOTHE,
	PET_INTENT_WITHDRAW,
	PET_INTENT_COUNT
};

/* ── Engine runtime (file-local, NOT in pet_state) ───────────────── */

struct behavior_runtime {
	/* Accumulator / session bookkeeping (unchanged) */
	uint8_t arousal_decay_accum_s;
	uint8_t ambient_energy_decay_accum;
	uint8_t five_min_accum;
	uint8_t thirty_min_accum;
	int32_t step_day_index;
	enum pet_expression forced_expression;
	bool forced_expression_active;
	bool step_day_valid;

	/* Afterglow context */
	struct behavior_context ctx;

	/* Intent */
	enum pet_intent intent;
	int16_t intent_strength;       /* 0..100 */
	int64_t intent_resolved_at_ms;
};

static struct behavior_runtime g_runtime;

/* ── Utility helpers ─────────────────────────────────────────────── */

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

static int8_t clamp_battery_percent(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 100) {
		return 100;
	}
	return (int8_t)value;
}

static int16_t clamp_look_range(int value)
{
	if (value < -100) {
		return -100;
	}
	if (value > 100) {
		return 100;
	}
	return (int16_t)value;
}

static uint8_t effective_walking_confidence(const struct pet_state *state)
{
	return MAX(state->walk_confidence, state->walking_confidence);
}

static bool pet_time_is_valid(const struct pet_state *state)
{
	return state->time_valid && (state->unix_time_at_sync > 0);
}

static int32_t current_local_day_index(const struct pet_state *state, int64_t now_ms)
{
	int64_t elapsed_s;
	int64_t local_s;

	elapsed_s = (now_ms - state->uptime_at_sync_ms) / MSEC_PER_SEC;
	local_s = state->unix_time_at_sync + elapsed_s + ((int64_t)state->tz_offset_minutes * 60);
	if (local_s < 0) {
		return 0;
	}

	return (int32_t)(local_s / 86400LL);
}

static void sync_daily_step_counter(struct pet_state *state, int64_t now_ms)
{
	int32_t local_day_index;

	if (!pet_time_is_valid(state)) {
		return;
	}

	local_day_index = current_local_day_index(state, now_ms);
	if (!g_runtime.step_day_valid) {
		g_runtime.step_day_index = local_day_index;
		g_runtime.step_day_valid = true;
		return;
	}

	if (local_day_index != g_runtime.step_day_index) {
		state->step_count_today = 0U;
		g_runtime.step_day_index = local_day_index;
		LOG_INF("Step day rollover -> local_day=%d", local_day_index);
	}
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
	state->trust = clamp_0_100(state->trust);
	state->curiosity = clamp_0_100(state->curiosity);
	state->walk_confidence = (uint8_t)clamp_0_100(state->walk_confidence);
	state->walking_confidence = (uint8_t)clamp_0_100(state->walking_confidence);
	if (state->walking_confidence == 0U) {
		state->walking_confidence = state->walk_confidence;
	} else if (state->walk_confidence == 0U) {
		state->walk_confidence = state->walking_confidence;
	}
	state->notification_burst_level = (uint8_t)clamp_0_100(state->notification_burst_level);
	state->look_confidence = (uint8_t)clamp_0_100(state->look_confidence);
	state->pickup_confidence = (uint8_t)clamp_0_100(state->pickup_confidence);
	state->in_hand_confidence = (uint8_t)clamp_0_100(state->in_hand_confidence);
	state->look_target_x = clamp_look_range(state->look_target_x);
	state->look_target_y = clamp_look_range(state->look_target_y);
	state->look_render_x = clamp_look_range(state->look_render_x);
	state->look_render_y = clamp_look_range(state->look_render_y);
	if (state->battery_percent_known) {
		state->battery_percent = clamp_battery_percent(state->battery_percent);
	} else {
		state->battery_percent = -1;
	}
}

static void mark_real_interaction(struct pet_state *state, int64_t now_ms)
{
	state->last_real_interaction_timestamp_ms = now_ms;
}

static bool is_night_time(const struct pet_state *state, int64_t now_ms)
{
	int64_t elapsed_s;
	int64_t local_s;
	int32_t day_s;
	int hour;

	/* Require a real wall-clock sync, not just a tz offset: otherwise a
	 * tz-only TIME_SYNC leaves unix_time_at_sync == 0 and we would compute
	 * the hour from epoch 0 and wrongly trigger night sleepiness. */
	if (!pet_time_is_valid(state)) {
		return false;
	}

	elapsed_s = (now_ms - state->uptime_at_sync_ms) / MSEC_PER_SEC;
	local_s = state->unix_time_at_sync + elapsed_s + ((int64_t)state->tz_offset_minutes * 60);
	day_s = (int32_t)(local_s % 86400);
	if (day_s < 0) {
		day_s += 86400;
	}

	hour = day_s / 3600;
	return (hour < 7) || (hour >= 22);
}

static void trigger_reaction(struct pet_state *state, enum micro_reaction_type reaction, int64_t now_ms)
{
	if (micro_reaction_trigger(reaction, now_ms)) {
		state->last_reaction_timestamp_ms = now_ms;
	}

	state->current_reaction = micro_reaction_get_active(now_ms);
}

static void show_social_indicator(struct pet_state *state,
				  enum kerfur_face_indicator_id id,
				  int64_t now_ms,
				  int32_t ttl_ms)
{
	if ((id <= KERFUR_FACE_INDICATOR_NONE) || (id >= KERFUR_FACE_INDICATOR_COUNT)) {
		return;
	}

	state->social_indicator = (int16_t)id;
	state->social_indicator_until_ms = now_ms + ttl_ms;
}

static void log_face_snapshot(const struct pet_state *state, const char *reason)
{
	LOG_INF("%s face mode=%s expr=%s force=%s react=%s ind=%d ov=%d look_t=%d,%d look_r=%d,%d look_c=%u carry=%d/%u/%u/%u batt=%d known=%d",
		reason,
		pet_mode_str(state->current_mode),
		pet_expression_str(state->current_expression),
		g_runtime.forced_expression_active ?
			pet_expression_str(g_runtime.forced_expression) : "AUTO",
		micro_reaction_str(state->current_reaction),
		state->current_indicator,
		state->current_overlay,
		state->look_target_x,
		state->look_target_y,
		state->look_render_x,
		state->look_render_y,
		state->look_confidence,
		state->in_hand ? 1 : 0,
		state->pickup_confidence,
		state->in_hand_confidence,
		state->walking_confidence,
		state->battery_percent,
		state->battery_percent_known ? 1 : 0);
}

/* ── Context (afterglow) management ──────────────────────────────── */

static void clamp_context(void)
{
	g_runtime.ctx.stimulation = (int16_t)CLAMP(g_runtime.ctx.stimulation, 0, 100);
	g_runtime.ctx.comfort = (int16_t)CLAMP(g_runtime.ctx.comfort, 0, 100);
	g_runtime.ctx.social_warmth = (int16_t)CLAMP(g_runtime.ctx.social_warmth, 0, 100);
}

static void decay_context(void)
{
	/* Called every 1 s from the tick handler. */
	g_runtime.ctx.stimulation -= 3;
	g_runtime.ctx.comfort -= 2;
	g_runtime.ctx.social_warmth -= 1;
	clamp_context();
}

/* ── Intent resolution ───────────────────────────────────────────── */

#define INTENT_RESOLVE_INTERVAL_MS 5000LL
#define INTENT_SWITCH_MARGIN       8

static const char *pet_intent_str(enum pet_intent intent)
{
	switch (intent) {
	case PET_INTENT_NONE:           return "NONE";
	case PET_INTENT_REST:           return "REST";
	case PET_INTENT_SEEK_ATTENTION: return "SEEK_ATTN";
	case PET_INTENT_OBSERVE:        return "OBSERVE";
	case PET_INTENT_PLAY:           return "PLAY";
	case PET_INTENT_SELF_SOOTHE:    return "SOOTHE";
	case PET_INTENT_WITHDRAW:       return "WITHDRAW";
	default:                        return "?";
	}
}

static void resolve_intent(struct pet_state *state, int64_t now_ms)
{
	int16_t scores[PET_INTENT_COUNT];
	enum pet_intent best;
	int16_t best_score;
	int16_t current_score;
	int64_t no_interaction_ms;
	int i;

	if ((now_ms - g_runtime.intent_resolved_at_ms) < INTENT_RESOLVE_INTERVAL_MS) {
		return;
	}
	g_runtime.intent_resolved_at_ms = now_ms;
	no_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;

	(void)memset(scores, 0, sizeof(scores));

	/* REST: sleepy + calm */
	if (state->sleepiness > 50 && state->arousal < 45) {
		scores[PET_INTENT_REST] = (int16_t)(
			(state->sleepiness - 35) +
			(45 - state->arousal) / 3 +
			(g_runtime.ctx.comfort > 30 ? 8 : 0));
	}

	/* SEEK_ATTENTION: bored or lonely */
	if (state->boredom > 35 || no_interaction_ms > (5LL * 60 * MSEC_PER_SEC)) {
		scores[PET_INTENT_SEEK_ATTENTION] = (int16_t)(
			state->boredom / 2 +
			(no_interaction_ms > (10LL * 60 * MSEC_PER_SEC) ? 15 : 0) +
			(no_interaction_ms > (5LL * 60 * MSEC_PER_SEC) ? 8 : 0));
	}

	/* OBSERVE: curious + moderate arousal */
	if (state->curiosity > 35 && state->arousal > 15) {
		scores[PET_INTENT_OBSERVE] = (int16_t)(
			state->curiosity / 2 +
			state->arousal / 5 +
			((g_runtime.ctx.stimulation > 15 && g_runtime.ctx.stimulation < 50) ? 6 : 0));
	}

	/* PLAY: energetic + trusting + not stressed */
	if (state->arousal > 45 && state->trust > 35 && state->stress < 45) {
		scores[PET_INTENT_PLAY] = (int16_t)(
			state->arousal / 2 +
			state->trust / 5 -
			state->stress / 4 +
			(state->energy > 40 ? 5 : 0));
	}

	/* SELF_SOOTHE: moderate stress + recent comfort */
	if (state->stress > 25 && g_runtime.ctx.comfort > 15) {
		scores[PET_INTENT_SELF_SOOTHE] = (int16_t)(
			state->stress / 3 +
			g_runtime.ctx.comfort / 3);
	}

	/* WITHDRAW: high stress or overstimulated */
	if (state->stress > 45 || g_runtime.ctx.stimulation > 55) {
		scores[PET_INTENT_WITHDRAW] = (int16_t)(
			state->stress / 2 +
			(g_runtime.ctx.stimulation > 55
				? (g_runtime.ctx.stimulation - 35) / 3 : 0) -
			(g_runtime.ctx.comfort > 30 ? 8 : 0));
	}

	/* Find best */
	best = PET_INTENT_NONE;
	best_score = 0;
	for (i = 1; i < PET_INTENT_COUNT; i++) {
		if (scores[i] > best_score) {
			best = (enum pet_intent)i;
			best_score = scores[i];
		}
	}

	/* Hysteresis: require margin to switch */
	if (best != g_runtime.intent) {
		current_score = scores[g_runtime.intent];
		if (best_score < current_score + INTENT_SWITCH_MARGIN) {
			g_runtime.intent_strength =
				(int16_t)CLAMP(current_score, 0, 100);
			return;
		}
	}

	if (g_runtime.intent != best) {
		LOG_INF("Intent: %s(%d) -> %s(%d)",
			pet_intent_str(g_runtime.intent), g_runtime.intent_strength,
			pet_intent_str(best), best_score);
	}
	g_runtime.intent = best;
	g_runtime.intent_strength = (int16_t)CLAMP(best_score, 0, 100);
}

/* ── Passive drift (tick handlers) ───────────────────────────────── */

static void apply_5min_drift(struct pet_state *state, int64_t now_ms)
{
	const int64_t no_rough_ms = now_ms - state->last_rough_event_timestamp_ms;
	const int64_t no_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;

	if ((state->current_mode == PET_MODE_ASLEEP) || state->charging) {
		state->energy += 1;
	}

	if (state->current_display_state == DISPLAY_FOREGROUND) {
		state->energy -= 1;
	}

	if (no_rough_ms >= (30LL * 60 * MSEC_PER_SEC)) {
		state->trust += 1;
	}

	if (no_interaction_ms >= (45LL * 60 * MSEC_PER_SEC)) {
		state->attachment -= 1;
	}

	if (is_night_time(state, now_ms)) {
		state->sleepiness += 1;
	}
}

static void apply_30min_drift(struct pet_state *state, int64_t now_ms)
{
	const int64_t no_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;
	const int64_t no_rough_ms = now_ms - state->last_rough_event_timestamp_ms;

	if (no_interaction_ms >= (2LL * 60 * 60 * MSEC_PER_SEC)) {
		state->attachment -= 1;
	}

	if (no_rough_ms >= (2LL * 60 * 60 * MSEC_PER_SEC)) {
		state->trust += 1;
	}
}

static void apply_tick_1s(struct pet_state *state, int64_t now_ms)
{
	/* Arousal decay */
	g_runtime.arousal_decay_accum_s++;
	if (g_runtime.arousal_decay_accum_s >= 2U) {
		g_runtime.arousal_decay_accum_s = 0U;
		state->arousal -= 1;
	}

	/* Walk confidence timeout */
	if ((state->current_mode == PET_MODE_WALK_AWAKE) &&
	    ((now_ms - state->last_walk_timestamp_ms) > (12 * MSEC_PER_SEC))) {
		state->walk_confidence = 0;
		state->walking_confidence = 0;
		state->walking_active = false;
	}

	if (state->walking_active &&
	    ((now_ms - state->last_walk_timestamp_ms) > (18 * MSEC_PER_SEC))) {
		state->walking_active = false;
	}

	/* Social indicator expiry */
	if ((state->social_indicator != 0) &&
	    (state->social_indicator_until_ms != 0) &&
	    (now_ms >= state->social_indicator_until_ms)) {
		const bool is_heart =
			(state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_FILLED) ||
			(state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE);

		if (state->peer_nearby && is_heart) {
			state->social_indicator_until_ms = now_ms + (10 * MSEC_PER_SEC);
		} else {
			state->social_indicator = 0;
			state->social_indicator_until_ms = 0;
		}
	}

	/* Walk-together social hint */
	if (state->walking_active && state->peer_nearby) {
		state->boredom -= 1;
		if ((now_ms - state->last_social_glance_ms) >= (8 * MSEC_PER_SEC)) {
			state->last_social_glance_ms = now_ms;
			trigger_reaction(state,
					 (now_ms & 1LL) ? REACTION_GLANCE_RIGHT
							: REACTION_GLANCE_LEFT,
					 now_ms);
		}
	}

	/* Context afterglow decay */
	decay_context();

	sync_daily_step_counter(state, now_ms);
}

static void apply_tick_10s(struct pet_state *state, int64_t now_ms)
{
	if ((now_ms - state->last_phone_event_timestamp_ms) > (20 * MSEC_PER_SEC)) {
		state->social_load -= 1;
	}

	if ((now_ms - state->last_rough_event_timestamp_ms) > (30 * MSEC_PER_SEC)) {
		state->stress -= 2;
	}

	if ((state->current_mode != PET_MODE_ASLEEP) && !state->charging) {
		if (state->current_display_state == DISPLAY_FOREGROUND) {
			state->energy -= 1;
		} else if (state->current_display_state == DISPLAY_AMBIENT) {
			g_runtime.ambient_energy_decay_accum++;
			if (g_runtime.ambient_energy_decay_accum >= 3U) {
				g_runtime.ambient_energy_decay_accum = 0U;
				state->energy -= 1;
			}
		}
	}

	/* Curiosity decay: interest fades when nothing new is happening.
	 * −1/10s = −6/min base.  Faster when truly idle. */
	if (g_runtime.ctx.stimulation < 15) {
		state->curiosity -= 2;
	} else {
		state->curiosity -= 1;
	}

	/* Attachment drift: slowly returns toward baseline (~50) when
	 * not reinforced by interaction.  Prevents permanent 100. */
	if (state->attachment > 50 &&
	    (now_ms - state->last_real_interaction_timestamp_ms) > (60 * MSEC_PER_SEC)) {
		state->attachment -= 1;
	}

	/* Context-influenced passive effects:
	 * Lingering comfort soothes residual stress. */
	if (g_runtime.ctx.comfort > 40 && state->stress > 0) {
		state->stress -= 1;
	}
	/* Social warmth alleviates boredom. */
	if (g_runtime.ctx.social_warmth > 30 && state->boredom > 0) {
		state->boredom -= 1;
	}
}

static void apply_tick_60s(struct pet_state *state, int64_t now_ms)
{
	const int64_t no_real_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;

	if ((state->current_mode != PET_MODE_ASLEEP) && !state->charging) {
		state->sleepiness += 1;
	} else {
		state->sleepiness -= 1;
	}

	if (state->current_mode == PET_MODE_WALK_AWAKE) {
		state->boredom -= 1;
	} else if (no_real_interaction_ms >= (5LL * 60 * MSEC_PER_SEC)) {
		state->boredom += 1;
	}

	if (state->walking_active && (no_real_interaction_ms >= (2LL * 60 * MSEC_PER_SEC))) {
		state->walking_active = false;
	}

	if (state->notification_burst_level > 0U) {
		state->notification_burst_level--;
	}

	/* Extra curiosity decay during long idle — prevents stale 100. */
	if (no_real_interaction_ms >= (3LL * 60 * MSEC_PER_SEC) && state->curiosity > 30) {
		state->curiosity -= 1;
	}

	g_runtime.five_min_accum++;
	g_runtime.thirty_min_accum++;

	if (g_runtime.five_min_accum >= 5U) {
		g_runtime.five_min_accum = 0U;
		apply_5min_drift(state, now_ms);
	}

	if (g_runtime.thirty_min_accum >= 30U) {
		g_runtime.thirty_min_accum = 0U;
		apply_30min_drift(state, now_ms);
	}
}

/* ── Event classification helpers ────────────────────────────────── */

static bool event_is_real_interaction(enum app_event_type type, const struct pet_state *state)
{
	switch (type) {
	case APP_EVENT_USER_TAP:
	case APP_EVENT_USER_PET_SOFT:
	case APP_EVENT_USER_PET_LONG:
	case APP_EVENT_USER_HOLD:
	case APP_EVENT_PICKED_UP:
	case APP_EVENT_IN_HAND_ENTER:
	case APP_EVENT_APP_SESSION_START:
	case APP_EVENT_CHARGER_CONNECTED:
		return true;
	case APP_EVENT_STEP_BATCH:
		return state->walking_active || (effective_walking_confidence(state) >= 60U);
	default:
		return false;
	}
}

static bool in_hand_motion_context_recent(const struct pet_state *state, int64_t now_ms)
{
	return state->in_hand ||
	       ((state->last_in_hand_timestamp_ms > 0LL) &&
		((now_ms - state->last_in_hand_timestamp_ms) <= 3000LL) &&
		(state->in_hand_confidence >= 30U));
}

/* ── Event interpretation + application ──────────────────────────── *
 *
 * Each event handler:
 *   1. Reads current emotional context (local const bools)
 *   2. Updates afterglow context (stimulation / comfort / social_warmth)
 *   3. Applies BASE stat deltas (close to original tuning)
 *   4. Applies CONTEXTUAL modifiers (small adjustments based on how
 *      the pet already feels)
 *   5. Triggers the most appropriate micro-reaction for the context
 */

static void apply_event(struct pet_state *state, const struct app_event *event)
{
	const int64_t now = event->timestamp_ms;
	int steps;

	switch (event->type) {

	/* ── Tick events ─────────────────────────────────────────────── */

	case APP_EVENT_TICK_100MS:
		micro_reaction_tick_100ms(now);
		state->current_reaction = micro_reaction_get_active(now);
		break;

	case APP_EVENT_TICK_1S:
		apply_tick_1s(state, now);
		break;

	case APP_EVENT_TICK_10S:
		apply_tick_10s(state, now);
		break;

	case APP_EVENT_TICK_60S:
		apply_tick_60s(state, now);
		break;

	/* ── Touch events ────────────────────────────────────────────── */

	case APP_EVENT_USER_TAP: {
		const bool drowsy = state->sleepiness > 60;
		const bool lonely = state->boredom > 50;

		g_runtime.ctx.stimulation += 5;
		g_runtime.ctx.comfort += 3;

		state->arousal += 2;
		state->boredom -= 1;

		if (drowsy) {
			/* Reluctant wake — the pet was resting */
			state->sleepiness -= 3;
			state->curiosity += 1;
			trigger_reaction(state, REACTION_WAKE_BLINK, now);
		} else if (lonely) {
			/* Happy to get attention */
			state->curiosity += 3;
			state->attachment += 2;
			trigger_reaction(state, REACTION_BLINK, now);
		} else {
			state->curiosity += 2;
			trigger_reaction(state, REACTION_BLINK, now);
		}
		break;
	}

	case APP_EVENT_USER_PET_SOFT: {
		const bool drowsy = state->sleepiness > 60;
		const bool stressed = state->stress > 40;
		const bool buzzing = g_runtime.ctx.stimulation > 55;
		const bool warm = g_runtime.ctx.comfort > 40;

		g_runtime.ctx.comfort += 20;
		g_runtime.ctx.stimulation += 6;
		state->last_pet_timestamp_ms = now;

		/* Base warmth and trust (preserved from original) */
		state->attachment += 8;
		state->trust += 6;
		state->stress -= 7;
		state->boredom -= 5;

		/* Contextual modulation */
		if (drowsy) {
			/* Soothing: pet settles into coziness */
			state->sleepiness += 2;
			state->stress -= 4;
			state->trust += 3;
			state->arousal -= 1;
		} else if (stressed) {
			/* Relief: drawing comfort from touch */
			state->stress -= 5;
			state->trust += 3;
			state->arousal -= 1;
		} else if (buzzing) {
			/* Overstimulated: nice but diminishing returns */
			state->attachment -= 3;
			state->arousal += 1;
		} else {
			/* Normal happy petting */
			state->sleepiness -= 2;
			state->arousal += 3;
			state->curiosity += 1;
		}

		/* Contextual reaction: already warm -> happy bounce */
		if (warm && !stressed) {
			trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		} else {
			trigger_reaction(state, REACTION_PET_BOW, now);
		}
		break;
	}

	case APP_EVENT_USER_PET_LONG: {
		const bool drowsy = state->sleepiness > 60;
		const bool stressed = state->stress > 40;

		g_runtime.ctx.comfort += 25;
		g_runtime.ctx.stimulation += 5;
		state->last_pet_timestamp_ms = now;

		state->attachment += 10;
		state->trust += 4;
		state->stress -= 6;
		state->boredom -= 4;

		if (drowsy) {
			/* Deep cozy — long petting while drowsy */
			state->sleepiness += 4;
			state->stress -= 5;
			state->trust += 4;
			state->arousal -= 2;
		} else if (stressed) {
			state->stress -= 6;
			state->trust += 4;
		} else {
			state->sleepiness -= 3;
			state->arousal += 2;
		}

		trigger_reaction(state, REACTION_PET_BOW, now);
		break;
	}

	case APP_EVENT_USER_HOLD: {
		g_runtime.ctx.comfort += 15;
		g_runtime.ctx.stimulation += 3;
		state->last_pet_timestamp_ms = now;

		state->attachment += 4;
		state->trust += 3;
		state->stress -= 2;
		state->boredom -= 2;

		/* Holding feels secure when anxious */
		if (state->stress > 30) {
			state->stress -= 2;
			state->trust += 2;
		}
		break;
	}

	/* ── Motion events ───────────────────────────────────────────── */

	case APP_EVENT_SHAKE_LIGHT: {
		const bool sleepy = state->sleepiness > 60;
		const bool carried = in_hand_motion_context_recent(state, now);

		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;

		if (carried) {
			/* In-hand jostling: barely registers emotionally */
			g_runtime.ctx.stimulation += 3;
			state->arousal += 1;
		} else {
			/* Bumped on a surface: noticeable */
			g_runtime.ctx.stimulation += 6;
			state->arousal += 3;
			state->curiosity += 2;
			state->sleepiness -= 2;
			state->stress += 1;

			if (sleepy) {
				trigger_reaction(state, REACTION_WAKE_BLINK, now);
			} else if (state->arousal <= 50) {
				trigger_reaction(state, REACTION_GLANCE_LEFT, now);
			}
		}
		break;
	}

	case APP_EVENT_SHAKE_PLAY: {
		const bool sleepy = state->sleepiness > 60;
		const bool stressed_already = state->stress > 50;
		const bool carried = in_hand_motion_context_recent(state, now);
		const bool playful = g_runtime.intent == PET_INTENT_PLAY ||
				     state->arousal > 45;

		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;

		if (carried) {
			/* Deliberate play shake while held */
			g_runtime.ctx.stimulation += 8;
			state->arousal += 4;
			state->curiosity += 2;
			state->boredom -= 2;
			state->sleepiness -= 3;
			state->stress += (state->trust >= 40) ? 0 : 2;

			if (playful && state->trust > 40) {
				state->boredom -= 2;
				trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
			} else if (stressed_already) {
				trigger_reaction(state, REACTION_STARTLE, now);
			}
		} else {
			/* Play shake on surface: more startling */
			g_runtime.ctx.stimulation += 10;
			state->arousal += 5;
			state->curiosity += 3;
			state->boredom -= 3;
			state->sleepiness -= 4;
			state->stress += (state->trust >= 40) ? 1 : 3;

			if (playful && state->trust > 40) {
				state->boredom -= 2;
			} else if (stressed_already) {
				state->stress += 2;
			}

			if (sleepy) {
				trigger_reaction(state, REACTION_WAKE_BLINK, now);
			} else if (stressed_already) {
				trigger_reaction(state, REACTION_STARTLE, now);
			} else {
				trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
			}
		}
		break;
	}

	case APP_EVENT_SHAKE_ROUGH: {
		const bool carried = in_hand_motion_context_recent(state, now);

		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		state->last_rough_event_timestamp_ms = now;
		state->pickup_confidence = (uint8_t)MAX(0, (int)state->pickup_confidence - 20);
		state->in_hand_confidence = (uint8_t)MAX(0, (int)state->in_hand_confidence - 25);
		state->walking_active = false;

		if (carried) {
			/* Rough handling while held: scary but trust-context applies */
			g_runtime.ctx.stimulation += 15;
			g_runtime.ctx.comfort -= 6;
			state->arousal += 5;
			state->sleepiness -= 5;
		} else {
			/* Rough shake on surface: more alarming */
			g_runtime.ctx.stimulation += 20;
			g_runtime.ctx.comfort -= 10;
			state->arousal += 7;
			state->sleepiness -= 7;
		}

		/* Trust-dependent damage: familiar handler vs stranger */
		if (state->trust >= 50) {
			state->stress += 5;
			state->trust -= 2;
			state->attachment -= 1;
		} else {
			state->stress += 10;
			state->trust -= 4;
			state->attachment -= 2;
		}

		trigger_reaction(state, REACTION_STARTLE, now);
		break;
	}

	case APP_EVENT_IMPACT: {
		g_runtime.ctx.stimulation += 15;
		g_runtime.ctx.comfort -= 4;

		state->stress += 6;
		state->trust -= 1;
		state->arousal += 5;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		state->last_rough_event_timestamp_ms = now;
		state->pickup_confidence = (uint8_t)MAX(0, (int)state->pickup_confidence - 15);
		state->in_hand_confidence = (uint8_t)MAX(0, (int)state->in_hand_confidence - 20);
		state->walking_active = false;

		/* Recent comfort buffers trust damage */
		if (g_runtime.ctx.comfort > 30) {
			state->trust += 1;
		}

		trigger_reaction(state, REACTION_STARTLE, now);
		break;
	}

	case APP_EVENT_MOTION_WAKE: {
		g_runtime.ctx.stimulation += 5;

		state->arousal += 2;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		if (!in_hand_motion_context_recent(state, now) &&
		    ((now - state->last_motion_wake_reaction_ms) >= 4000LL)) {
			trigger_reaction(state, REACTION_WAKE_BLINK, now);
			state->last_motion_wake_reaction_ms = now;
		}
		break;
	}

	case APP_EVENT_WALKING_START: {
		const bool was_bored = state->boredom > 40;
		const bool carried = state->in_hand ||
				     state->in_hand_confidence > 50;

		state->walk_confidence = (uint8_t)MAX(state->walk_confidence, 80);
		state->walking_confidence = (uint8_t)MAX(state->walking_confidence, 80);
		state->walking_active = true;
		state->walking_session_start_ms = now;
		state->last_walk_timestamp_ms = now;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;

		if (carried) {
			/* Being carried: pleasant but passive transport */
			g_runtime.ctx.stimulation += 4;
			g_runtime.ctx.comfort += 4;
			state->attachment += 1;
		} else {
			/* Actually walking: exploring the world */
			g_runtime.ctx.stimulation += 8;
			if (was_bored) {
				state->curiosity += 4;
				state->boredom -= 3;
			}
			trigger_reaction(state, REACTION_LOOK_UP, now);
		}
		break;
	}

	case APP_EVENT_WALKING_STOP:
		state->walking_confidence = (uint8_t)MAX(0, event->param);
		state->walk_confidence = state->walking_confidence;
		state->walking_active = false;
		state->last_walk_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		break;

	case APP_EVENT_STEP_BATCH: {
		int boredom_factor;
		int curiosity_add;
		const bool carried = state->in_hand ||
				     state->in_hand_confidence > 50;

		g_runtime.ctx.stimulation += carried ? 1 : 2;

		steps = MAX(event->param, 0);

		/* Diminishing boredom reduction: first 500 steps most impactful */
		boredom_factor = (state->step_count_today < 500U) ? 3 :
				 (state->step_count_today < 2000U) ? 2 : 1;

		if (carried) {
			/* Being carried: passive motion, reduced exploration gains */
			state->boredom -= MIN((steps * boredom_factor / 4), 4);
			/* No curiosity gain — the pet isn't exploring, it's along for the ride */
			curiosity_add = 0;
		} else {
			/* Walking: full exploration benefit */
			state->boredom -= MIN((steps * boredom_factor / 2), 8);
			/* Curiosity: more impactful early in walk */
			curiosity_add = (state->walking_session_start_ms > 0LL &&
				(now - state->walking_session_start_ms) < (120LL * MSEC_PER_SEC))
				? MIN(steps, 4) : 1;
		}

		state->curiosity += curiosity_add;
		state->sleepiness -= carried ? 1 : 2;

		if (state->walking_active || (effective_walking_confidence(state) >= 60U)) {
			state->walking_active = true;
		}
		/* Walking together with phone: gentle attachment, but only
		 * when attachment isn't already saturated (prevents ratchet). */
		if (state->ble_connected && (steps > 0) && state->attachment < 70) {
			state->attachment += 1;
		}
		if (steps > 0) {
			state->step_count_today += (uint32_t)steps;
			state->total_steps_since_boot += (uint32_t)steps;
			state->last_motion_timestamp_ms = now;
			state->last_walk_timestamp_ms = now;
			state->last_motion_sample_timestamp_ms = now;
			if (event->payload.step_batch.from_hw_counter) {
				state->last_hw_step_counter = event->payload.step_batch.hw_counter;
			}
			state->walk_confidence =
				(uint8_t)MIN(100, MAX(state->walk_confidence,
						       event->payload.step_batch.walking_confidence));
			state->walking_confidence = state->walk_confidence;
			state->walking_active = state->walking_confidence >= 70U;
			if (state->walking_active && (state->walking_session_start_ms == 0LL)) {
				state->walking_session_start_ms = now;
			}
		}
		break;
	}

	case APP_EVENT_PICKUP_CANDIDATE:
		g_runtime.ctx.stimulation += 5;
		state->pickup_confidence = (uint8_t)MAX(state->pickup_confidence,
						      (uint8_t)MAX(event->param, 0));
		state->picked_up_recently = true;
		state->last_pickup_timestamp_ms = now;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		break;

	case APP_EVENT_PICKED_UP: {
		g_runtime.ctx.stimulation += 8;
		g_runtime.ctx.comfort += 5;

		state->pickup_confidence = (uint8_t)MAX(state->pickup_confidence,
						      (uint8_t)MAX(event->param, 0));
		state->picked_up_recently = true;
		state->last_pickup_timestamp_ms = now;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		state->arousal += 2;
		state->curiosity += 2;
		state->attachment += 2;
		break;
	}

	case APP_EVENT_IN_HAND_ENTER:
		g_runtime.ctx.stimulation += 5;
		g_runtime.ctx.comfort += 8;

		state->in_hand = true;
		state->picked_up_recently = true;
		state->in_hand_confidence = (uint8_t)MAX(state->in_hand_confidence,
							(uint8_t)MAX(event->param, 0));
		state->last_in_hand_timestamp_ms = now;
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		state->sleepiness -= 1;
		state->curiosity += 2;
		state->attachment += 2;
		break;

	case APP_EVENT_IN_HAND_EXIT:
		state->in_hand = false;
		state->in_hand_confidence = (uint8_t)MIN(state->in_hand_confidence,
							(uint8_t)MAX(0, 100 - MAX(event->param, 0)));
		state->last_motion_timestamp_ms = now;
		state->last_motion_sample_timestamp_ms = now;
		break;

	case APP_EVENT_FLIP_FACE_DOWN:
		g_runtime.ctx.stimulation += 3;
		state->sleepiness += 2;
		trigger_reaction(state, REACTION_LOOK_DOWN, now);
		break;

	/* ── Phone / notification events ─────────────────────────────── */

	case APP_EVENT_PHONE_NOTIFICATION_SINGLE: {
		const bool bored = state->boredom > 40;
		const bool stressed = state->stress > 45;

		g_runtime.ctx.stimulation += 8;
		g_runtime.ctx.social_warmth += 5;
		state->last_phone_event_timestamp_ms = now;

		state->social_load += 5;
		state->curiosity += 6;
		state->arousal += 4;
		state->boredom -= 2;

		if (bored) {
			/* Welcome distraction */
			state->curiosity += 3;
			state->boredom -= 2;
		} else if (stressed) {
			/* Another thing to deal with */
			state->stress += 2;
			state->curiosity -= 2;
		}

		trigger_reaction(state, REACTION_NOTIF_PING, now);
		break;
	}

	case APP_EVENT_PHONE_NOTIFICATION_BURST: {
		const bool already_loaded = state->social_load > 50 ||
					    g_runtime.ctx.stimulation > 40;

		g_runtime.ctx.stimulation += 18;
		g_runtime.ctx.social_warmth += 8;
		state->last_phone_event_timestamp_ms = now;

		state->social_load += 15;
		state->arousal += 8;
		state->boredom -= 6;
		state->notification_burst_level =
			(uint8_t)MIN(100, state->notification_burst_level + 1);

		if (already_loaded) {
			/* Overload: this is too much */
			state->stress += 12;
			state->curiosity -= 2;
		} else {
			/* First burst: stressful but manageable */
			state->stress += 8;
			state->curiosity += 2;
		}

		trigger_reaction(state, REACTION_NOTIF_BURST, now);
		break;
	}

	case APP_EVENT_PHONE_CONNECTED: {
		g_runtime.ctx.stimulation += 6;
		g_runtime.ctx.social_warmth += 10;

		state->ble_connected = true;
		state->curiosity += 3;
		state->social_load += 2;
		state->boredom -= 1;
		state->last_phone_event_timestamp_ms = now;

		/* Reconnecting when lonely is a relief */
		if (state->boredom > 40) {
			state->boredom -= 3;
			state->attachment += 2;
		}

		trigger_reaction(state, REACTION_CONNECT_SPARK, now);
		break;
	}

	case APP_EVENT_PHONE_DISCONNECTED:
		g_runtime.ctx.stimulation += 5;
		g_runtime.ctx.social_warmth -= 5;

		state->ble_connected = false;
		state->boredom += 3;
		state->curiosity += 1;
		state->last_phone_event_timestamp_ms = now;
		trigger_reaction(state, REACTION_DISCONNECT_SCAN, now);
		break;

	case APP_EVENT_APP_SESSION_START:
		g_runtime.ctx.stimulation += 5;
		g_runtime.ctx.social_warmth += 8;

		state->app_session_active = true;
		state->curiosity += 4;
		state->trust += 2;
		trigger_reaction(state, REACTION_CONNECT_SPARK, now);
		break;

	case APP_EVENT_APP_SESSION_END:
		state->app_session_active = false;
		break;

	case APP_EVENT_TIME_SYNC:
		state->time_valid = true;
		state->uptime_at_sync_ms = now;
		if ((event->param >= -840) && (event->param <= 840)) {
			state->tz_offset_minutes = (int16_t)event->param;
		} else if (event->param >= 946684800) {
			state->unix_time_at_sync = event->param;
		}
		sync_daily_step_counter(state, now);
		break;

	/* ── Power events ────────────────────────────────────────────── */

	case APP_EVENT_CHARGER_CONNECTED:
		g_runtime.ctx.stimulation += 3;
		g_runtime.ctx.comfort += 8;

		state->charging = true;
		state->stress -= 4;
		state->sleepiness += 2;
		state->curiosity -= 1;
		trigger_reaction(state, REACTION_CHARGE_PULSE, now);
		break;

	case APP_EVENT_CHARGER_DISCONNECTED:
		state->charging = false;
		state->curiosity += 2;
		break;

	case APP_EVENT_BATTERY_LOW:
		state->battery_low = true;
		state->stress += 5;
		state->sleepiness += 4;
		trigger_reaction(state, REACTION_LOW_BATT_SAG, now);
		break;

	case APP_EVENT_BATTERY_CRITICAL:
		state->battery_critical = true;
		state->battery_low = true;
		state->ambient_wake_enabled = false;
		state->stress += 8;
		state->sleepiness += 8;
		break;

	/* ── Social / peer events ────────────────────────────────────── */

	case APP_EVENT_PEER_SEEN:
		state->curiosity += 2;
		break;

	case APP_EVENT_PEER_CHECKING:
		show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_QUESTION,
				      now, 1500);
		break;

	case APP_EVENT_PEER_NEAR: {
		const bool is_friend = event->payload.peer.is_friend;

		state->peer_nearby = true;
		state->current_active_peer_id = event->payload.peer.ephemeral_id;
		state->peer_known_friend = is_friend;
		state->social_load += 1;

		if (is_friend) {
			g_runtime.ctx.social_warmth += 25;
			g_runtime.ctx.comfort += 10;
			state->curiosity += 5;
			state->attachment += 2;
		} else {
			g_runtime.ctx.social_warmth += 15;
			g_runtime.ctx.stimulation += 5;
			state->curiosity += 6;
		}

		show_social_indicator(state,
				      is_friend ? KERFUR_FACE_INDICATOR_ICON_HEART_FILLED
					       : KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
				      now, 10 * MSEC_PER_SEC);

		if (!state->social_overload &&
		    ((now - state->last_social_glance_ms) >= (4 * MSEC_PER_SEC))) {
			state->last_social_glance_ms = now;
			trigger_reaction(state, REACTION_GLANCE_LEFT, now);
		}
		break;
	}

	case APP_EVENT_PEER_LOST:
		g_runtime.ctx.social_warmth -= 10;

		if (state->current_active_peer_id == event->payload.peer.ephemeral_id) {
			state->peer_nearby = false;
			state->peer_known_friend = false;
			state->current_active_peer_id = 0U;
		}
		if (state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE ||
		    state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_FILLED) {
			state->social_indicator = 0;
			state->social_indicator_until_ms = 0;
		}
		break;

	case APP_EVENT_ENCOUNTER_START: {
		const bool is_friend = event->payload.peer.is_friend;

		state->peer_nearby = true;
		state->current_active_peer_id = event->payload.peer.ephemeral_id;
		state->peer_known_friend = is_friend;

		if (is_friend) {
			g_runtime.ctx.social_warmth += 30;
			g_runtime.ctx.comfort += 15;
			g_runtime.ctx.stimulation += 8;

			state->attachment += 6;
			state->trust += 3;
			state->stress -= 2;

			/* Reuniting when bored is extra warm */
			if (state->boredom > 40) {
				state->boredom -= 8;
				state->attachment += 3;
			}

			show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_FILLED,
					      now, 3000);
			trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		} else {
			g_runtime.ctx.social_warmth += 20;
			g_runtime.ctx.stimulation += 10;

			state->curiosity += 6;
			state->attachment += 2;

			/* Observing intent amplifies curiosity */
			if (g_runtime.intent == PET_INTENT_OBSERVE) {
				state->curiosity += 3;
			}

			show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
					      now, 3000);
			trigger_reaction(state, REACTION_PET_BOW, now);
		}
		state->encounter_sync_pending = true;
		break;
	}

	case APP_EVENT_ENCOUNTER_END:
		g_runtime.ctx.social_warmth -= 15;

		if (event->payload.peer.duration_s >= 30) {
			state->boredom += 4;
		}
		if (state->current_active_peer_id == event->payload.peer.ephemeral_id) {
			state->peer_nearby = false;
			state->peer_known_friend = false;
			state->current_active_peer_id = 0U;
		}
		state->social_indicator = 0;
		state->social_indicator_until_ms = 0;
		trigger_reaction(state, REACTION_GLANCE_RIGHT, now);
		break;

	case APP_EVENT_PEER_PLAY_INVITE:
		g_runtime.ctx.stimulation += 10;
		g_runtime.ctx.social_warmth += 12;

		state->arousal += 8;
		state->boredom -= 4;
		state->curiosity += 2;
		show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
				      now, 2500);
		trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		break;

	case APP_EVENT_PEER_PLAY_ACK:
		g_runtime.ctx.stimulation += 8;
		g_runtime.ctx.social_warmth += 10;

		state->arousal += 6;
		state->boredom -= 3;
		state->attachment += 2;
		trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		break;

	/* ── System / lifecycle events ───────────────────────────────── */

	case APP_EVENT_WAKE:
		g_runtime.ctx.stimulation += 10;

		state->sleepiness -= 8;
		state->arousal += 5;
		trigger_reaction(state, REACTION_WAKE_BLINK, now);
		break;

	case APP_EVENT_SLEEP_REQUEST:
		state->arousal -= 6;
		trigger_reaction(state, REACTION_SLEEP_FADE, now);
		break;

	case APP_EVENT_SELF_WAKE_TIMER:
		g_runtime.ctx.stimulation += 3;

		state->arousal += 1;
		state->curiosity += 1;
		state->last_self_wake_timestamp_ms = now;
		trigger_reaction(state, REACTION_GLANCE_LEFT, now);
		break;

	/* ── Sensor / look / carry pass-through ──────────────────────── */

	case APP_EVENT_LOOK_TARGET_UPDATE:
		state->look_target_x = event->payload.look_target.x;
		state->look_target_y = event->payload.look_target.y;
		state->look_confidence = event->payload.look_target.confidence;
		state->look_render_x = state->look_target_x;
		state->look_render_y = state->look_target_y;
		if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG_VERBOSE)) {
			LOG_INF("Face look target -> x=%d y=%d conf=%u",
				state->look_target_x, state->look_target_y,
				state->look_confidence);
		}
		break;

	case APP_EVENT_CARRY_STATE_UPDATE:
		state->in_hand = event->payload.carry_state.in_hand;
		state->pickup_confidence = event->payload.carry_state.pickup_confidence;
		state->in_hand_confidence = event->payload.carry_state.in_hand_confidence;
		state->walking_confidence = event->payload.carry_state.walking_confidence;
		state->walk_confidence = event->payload.carry_state.walking_confidence;
		if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG_VERBOSE)) {
			LOG_INF("Face carry -> in_hand=%d pickup=%u in_hand_conf=%u walk_conf=%u",
				state->in_hand ? 1 : 0, state->pickup_confidence,
				state->in_hand_confidence, state->walking_confidence);
		}
		break;

	case APP_EVENT_BATTERY_PERCENT_UPDATE:
		state->battery_percent_known = event->payload.battery_percent.known;
		state->battery_percent = event->payload.battery_percent.known ?
					 event->payload.battery_percent.percent : -1;
		if (event->payload.battery_percent.known) {
			state->battery_low = event->payload.battery_percent.percent <= 20;
			state->battery_critical = event->payload.battery_percent.percent <= 5;
		}
		if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG_VERBOSE)) {
			LOG_INF("Face battery -> percent=%d known=%d low=%d critical=%d",
				state->battery_percent,
				state->battery_percent_known ? 1 : 0,
				state->battery_low ? 1 : 0,
				state->battery_critical ? 1 : 0);
		}
		break;

	/* ── Debug / override events ─────────────────────────────────── */

	case APP_EVENT_FACE_FORCE_EXPRESSION:
		if ((event->param < PET_EXPR_CALM) || (event->param >= PET_EXPR_COUNT)) {
			LOG_WRN("Ignoring invalid forced expression id=%d", event->param);
			break;
		}

		g_runtime.forced_expression = (enum pet_expression)event->param;
		g_runtime.forced_expression_active = true;
		if (state->current_expression != g_runtime.forced_expression) {
			state->current_expression = g_runtime.forced_expression;
			state->last_expression_change_timestamp_ms = now;
		}
		LOG_INF("Forced expression -> %s",
			pet_expression_str(g_runtime.forced_expression));
		break;

	case APP_EVENT_FACE_CLEAR_FORCED_EXPRESSION:
		g_runtime.forced_expression_active = false;
		LOG_INF("Forced expression cleared");
		break;

	case APP_EVENT_FACE_TRIGGER_REACTION:
		if ((event->param <= REACTION_NONE) || (event->param >= REACTION_COUNT)) {
			LOG_WRN("Ignoring invalid reaction trigger id=%d", event->param);
			break;
		}

		trigger_reaction(state, (enum micro_reaction_type)event->param, now);
		LOG_INF("Triggered reaction -> %s",
			micro_reaction_str((enum micro_reaction_type)event->param));
		break;

	case APP_EVENT_FACE_DEBUG_DUMP:
		log_face_snapshot(state, "Face dump");
		break;

	case APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG:
		state->dynamic_pupils_forced_disabled = event->param != 0;
		LOG_INF("Dynamic pupils debug override -> %s",
			state->dynamic_pupils_forced_disabled ? "disabled" : "enabled");
		break;

	default:
		break;
	}

	/* Common post-processing */
	sync_daily_step_counter(state, now);
	if (event_is_real_interaction(event->type, state)) {
		mark_real_interaction(state, now);
	}
}

/* ── Mode resolution ─────────────────────────────────────────────── */

static void update_mode(struct pet_state *state, int64_t now_ms)
{
	const int64_t idle_ms = now_ms - state->last_real_interaction_timestamp_ms;
	const int64_t walk_recent_ms = now_ms - state->last_walk_timestamp_ms;
	const int64_t phone_recent_ms = now_ms - state->last_phone_event_timestamp_ms;
	const int64_t self_wake_recent_ms = now_ms - state->last_self_wake_timestamp_ms;
	enum pet_mode mode = PET_MODE_IDLE;

	/* Intent-adjusted thresholds:
	 * REST intent → drowsy sooner.  WITHDRAW intent → overload sooner. */
	int drowsy_threshold = 72;
	int overload_social_threshold = 80;

	if (g_runtime.intent == PET_INTENT_REST && g_runtime.intent_strength > 20) {
		drowsy_threshold = 62;
	}
	if (g_runtime.intent == PET_INTENT_WITHDRAW && g_runtime.intent_strength > 20) {
		overload_social_threshold = 65;
	}

	if (state->charging) {
		mode = PET_MODE_CHARGING;
	} else if (state->battery_critical) {
		mode = (state->sleepiness > 85) ? PET_MODE_ASLEEP : PET_MODE_LOW_POWER;
	} else if (state->social_load > overload_social_threshold ||
		   state->notification_burst_level > 6) {
		mode = PET_MODE_OVERLOADED;
	} else if (phone_recent_ms < (30 * MSEC_PER_SEC)) {
		mode = PET_MODE_TASK_ALERT;
	} else if (state->app_session_active) {
		mode = PET_MODE_INTERACTING;
	} else if ((state->walking_active ||
		   ((walk_recent_ms <= (12 * MSEC_PER_SEC)) &&
		    (effective_walking_confidence(state) >= 30U))) &&
		   !(state->in_hand || state->in_hand_confidence > 50)) {
		mode = PET_MODE_WALK_AWAKE;
	} else if (state->walking_active &&
		   (state->in_hand || state->in_hand_confidence > 50)) {
		/* Being carried: counts as interaction, not a walk */
		mode = PET_MODE_INTERACTING;
	} else if (self_wake_recent_ms < (20 * MSEC_PER_SEC)) {
		mode = PET_MODE_INTERACTING;
	} else if ((state->sleepiness > 88) && (state->arousal < 25) &&
		   (state->current_display_state == DISPLAY_OFF)) {
		mode = PET_MODE_ASLEEP;
	} else if (state->sleepiness > drowsy_threshold) {
		mode = PET_MODE_DROWSY;
	} else if (idle_ms < (45 * MSEC_PER_SEC)) {
		mode = PET_MODE_INTERACTING;
	} else if (state->battery_low) {
		mode = PET_MODE_LOW_POWER;
	}

	state->current_mode = mode;
}

/* ── Expression scoring ──────────────────────────────────────────── *
 *
 * Each expression gets:
 *   base      — original scoring from pet_state variables (preserved)
 *   ctx_bonus — afterglow context modulation (comfort / stimulation / warmth)
 *   int_bonus — intent alignment bonus
 */

static int score_for_expression(enum pet_expression expr, const struct pet_state *state, int64_t now_ms)
{
	const bool recent_pet = (now_ms - state->last_pet_timestamp_ms) < (10LL * 60 * MSEC_PER_SEC);
	const bool fresh_pet = (now_ms - state->last_pet_timestamp_ms) < (8 * MSEC_PER_SEC);
	const bool recent_motion = (now_ms - state->last_motion_timestamp_ms) < (45 * MSEC_PER_SEC);
	const bool recent_notif = (now_ms - state->last_phone_event_timestamp_ms) < (30 * MSEC_PER_SEC);
	const bool rough_recent = (now_ms - state->last_rough_event_timestamp_ms) < (60 * MSEC_PER_SEC);
	const int64_t no_real_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;
	const bool long_disconnect = !state->ble_connected &&
				     ((now_ms - state->last_phone_event_timestamp_ms) > (30LL * 60 * MSEC_PER_SEC));
	const bool night = is_night_time(state, now_ms);
	int base = 0;
	int ctx_bonus = 0;
	int int_bonus = 0;
	const int16_t is = g_runtime.intent_strength;

	/* ── Base scoring (original logic preserved) ── */

	switch (expr) {
	case PET_EXPR_CALM:
		base = 60 - state->stress - (state->social_load / 2) -
		       (state->boredom / 4) + (state->energy / 4);
		/* Context: calm benefits from low recent stimulation */
		if (g_runtime.ctx.stimulation < 15) {
			ctx_bonus = 5;
		}
		break;

	case PET_EXPR_CURIOUS: {
		int walk_bonus = 0;

		if (state->current_mode == PET_MODE_WALK_AWAKE) {
			int64_t walk_s = (now_ms - state->walking_session_start_ms) / MSEC_PER_SEC;

			if (walk_s < 120) {
				walk_bonus = 18;
			} else if (walk_s < 600) {
				walk_bonus = 18 - (int)((walk_s - 120) * 14 / 480);
			} else {
				walk_bonus = 4;
			}
		}
		base = state->curiosity + (recent_notif ? 16 : 0) + (recent_motion ? 12 : 0) +
		       walk_bonus + (state->app_session_active ? 8 : 0) - (state->sleepiness / 3);
		/* Context: moderate stimulation feeds curiosity */
		if (g_runtime.ctx.stimulation > 15 && g_runtime.ctx.stimulation < 55) {
			ctx_bonus = g_runtime.ctx.stimulation / 7;
		}
		break;
	}

	case PET_EXPR_CONTENT:
		base = (state->attachment / 2) + (state->trust / 2) + (recent_pet ? 16 : 0) -
		       (state->stress / 2);
		/* Context: recent comfort boosts contentment */
		ctx_bonus = g_runtime.ctx.comfort / 5;
		break;

	case PET_EXPR_HAPPY:
		base = (state->attachment / 2) + (state->energy / 3) + (recent_pet ? 18 : 0) +
		       (fresh_pet ? 26 : 0) - (state->stress / 2);
		/* Context: comfort afterglow lingers as happiness */
		ctx_bonus = g_runtime.ctx.comfort / 5;
		break;

	case PET_EXPR_PLAYFUL: {
		int walk_bonus = 0;

		if (state->current_mode == PET_MODE_WALK_AWAKE) {
			int64_t walk_s = (now_ms - state->walking_session_start_ms) / MSEC_PER_SEC;

			if (walk_s < 120) {
				walk_bonus = 14;
			} else if (walk_s < 600) {
				walk_bonus = 14 - (int)((walk_s - 120) * 10 / 480);
			} else {
				walk_bonus = 4;
			}
		}
		base = state->arousal + (state->trust / 3) + walk_bonus -
		       (state->battery_low ? 20 : 0);
		/* Context: moderate stimulation supports playfulness */
		if (g_runtime.ctx.stimulation > 25 && g_runtime.ctx.stimulation < 65) {
			ctx_bonus = g_runtime.ctx.stimulation / 8;
		}
		break;
	}

	case PET_EXPR_SLEEPY:
		base = state->sleepiness + ((state->arousal < 35) ? 8 : 0) + (night ? 6 : 0) +
		       ((no_real_interaction_ms > (5LL * 60 * MSEC_PER_SEC)) ? 10 : 0);
		break;

	case PET_EXPR_NEEDY:
		base = state->boredom + (state->attachment / 4) + (recent_pet ? -10 : 12) +
		       ((no_real_interaction_ms > (8LL * 60 * MSEC_PER_SEC)) ? 10 : 0);
		/* Context: recent social warmth and comfort dampen neediness */
		ctx_bonus = -(g_runtime.ctx.social_warmth / 4) -
			    (g_runtime.ctx.comfort / 5);
		break;

	case PET_EXPR_LONELY:
		base = state->boredom + (long_disconnect ? 20 : 0) + (recent_motion ? -8 : 8) +
		       ((no_real_interaction_ms > (20LL * 60 * MSEC_PER_SEC)) ? 16 : 0);
		/* Context: social warmth and comfort dampen loneliness */
		ctx_bonus = -(g_runtime.ctx.social_warmth / 4) -
			    (g_runtime.ctx.comfort / 5);
		break;

	case PET_EXPR_ANNOYED:
		base = state->stress + (rough_recent ? 24 : 0) + ((state->trust < 40) ? 12 : 0);
		/* Context: high stimulation feeds annoyance */
		if (g_runtime.ctx.stimulation > 65) {
			ctx_bonus = (g_runtime.ctx.stimulation - 45) / 5;
		}
		break;

	case PET_EXPR_OVERSTIMULATED:
		base = state->social_load + (state->notification_burst_level * 10) +
		       (state->arousal / 2);
		/* Context: recent stimulation overload */
		if (g_runtime.ctx.stimulation > 55) {
			ctx_bonus = (g_runtime.ctx.stimulation - 35) / 4;
		}
		break;

	case PET_EXPR_COZY:
		base = (state->charging ? 60 : 0) + ((100 - state->stress) / 2) +
		       (recent_pet ? 10 : 0);
		/* Context: comfort afterglow strongly boosts cozy */
		ctx_bonus = g_runtime.ctx.comfort / 4;
		break;

	case PET_EXPR_DRAINED:
		base = (state->battery_critical ? 80 : 0) + (state->battery_low ? 40 : 0) +
		       ((100 - state->energy) / 2) + (state->sleepiness / 2);
		break;

	case PET_EXPR_ASLEEP:
		base = (state->current_mode == PET_MODE_ASLEEP ? 100 : 0) +
		       (state->current_display_state == DISPLAY_OFF ? 20 : 0);
		break;

	default:
		return 0;
	}

	/* ── Intent alignment bonus ── */

	switch (g_runtime.intent) {
	case PET_INTENT_REST:
		if (expr == PET_EXPR_SLEEPY) {
			int_bonus = is / 6;
		} else if (expr == PET_EXPR_COZY) {
			int_bonus = is / 7;
		} else if (expr == PET_EXPR_CALM) {
			int_bonus = is / 9;
		}
		break;
	case PET_INTENT_PLAY:
		if (expr == PET_EXPR_PLAYFUL) {
			int_bonus = is / 5;
		} else if (expr == PET_EXPR_HAPPY) {
			int_bonus = is / 7;
		}
		break;
	case PET_INTENT_OBSERVE:
		if (expr == PET_EXPR_CURIOUS) {
			int_bonus = is / 5;
		} else if (expr == PET_EXPR_CALM) {
			int_bonus = is / 9;
		}
		break;
	case PET_INTENT_SEEK_ATTENTION:
		if (expr == PET_EXPR_NEEDY) {
			int_bonus = is / 6;
		} else if (expr == PET_EXPR_LONELY) {
			int_bonus = is / 8;
		}
		break;
	case PET_INTENT_WITHDRAW:
		if (expr == PET_EXPR_ANNOYED) {
			int_bonus = is / 6;
		} else if (expr == PET_EXPR_OVERSTIMULATED) {
			int_bonus = is / 7;
		}
		break;
	case PET_INTENT_SELF_SOOTHE:
		if (expr == PET_EXPR_CONTENT) {
			int_bonus = is / 6;
		} else if (expr == PET_EXPR_CALM) {
			int_bonus = is / 7;
		}
		break;
	default:
		break;
	}

	return base + ctx_bonus + int_bonus;
}

/* Emotional adjacency: returns a small score bonus (0..5) for expressions
 * that feel like natural transitions from the current one.  Prevents
 * jarring jumps like SLEEPY → PLAYFUL and encourages gradual progressions. */
static int transition_affinity(enum pet_expression current, enum pet_expression candidate)
{
	switch (current) {
	case PET_EXPR_SLEEPY:
		if (candidate == PET_EXPR_CALM) {
			return 5;
		}
		if (candidate == PET_EXPR_CURIOUS) {
			return 3;
		}
		break;
	case PET_EXPR_CALM:
		if (candidate == PET_EXPR_CURIOUS) {
			return 4;
		}
		if (candidate == PET_EXPR_CONTENT) {
			return 4;
		}
		if (candidate == PET_EXPR_SLEEPY) {
			return 3;
		}
		break;
	case PET_EXPR_CURIOUS:
		if (candidate == PET_EXPR_PLAYFUL) {
			return 4;
		}
		if (candidate == PET_EXPR_CALM) {
			return 3;
		}
		if (candidate == PET_EXPR_HAPPY) {
			return 3;
		}
		break;
	case PET_EXPR_PLAYFUL:
		if (candidate == PET_EXPR_HAPPY) {
			return 5;
		}
		if (candidate == PET_EXPR_CURIOUS) {
			return 3;
		}
		if (candidate == PET_EXPR_OVERSTIMULATED) {
			return 3;
		}
		break;
	case PET_EXPR_HAPPY:
		if (candidate == PET_EXPR_CONTENT) {
			return 5;
		}
		if (candidate == PET_EXPR_PLAYFUL) {
			return 3;
		}
		break;
	case PET_EXPR_CONTENT:
		if (candidate == PET_EXPR_CALM) {
			return 4;
		}
		if (candidate == PET_EXPR_HAPPY) {
			return 3;
		}
		break;
	case PET_EXPR_ANNOYED:
		if (candidate == PET_EXPR_CALM) {
			return 4;
		}
		if (candidate == PET_EXPR_OVERSTIMULATED) {
			return 3;
		}
		break;
	case PET_EXPR_OVERSTIMULATED:
		if (candidate == PET_EXPR_ANNOYED) {
			return 4;
		}
		if (candidate == PET_EXPR_CALM) {
			return 3;
		}
		break;
	case PET_EXPR_NEEDY:
		if (candidate == PET_EXPR_LONELY) {
			return 4;
		}
		if (candidate == PET_EXPR_CALM) {
			return 3;
		}
		break;
	case PET_EXPR_LONELY:
		if (candidate == PET_EXPR_NEEDY) {
			return 3;
		}
		if (candidate == PET_EXPR_CALM) {
			return 4;
		}
		break;
	case PET_EXPR_COZY:
		if (candidate == PET_EXPR_SLEEPY) {
			return 4;
		}
		if (candidate == PET_EXPR_CONTENT) {
			return 3;
		}
		break;
	case PET_EXPR_DRAINED:
		if (candidate == PET_EXPR_SLEEPY) {
			return 5;
		}
		if (candidate == PET_EXPR_COZY) {
			return 3;
		}
		break;
	default:
		break;
	}
	return 0;
}

/* ── Expression resolution ───────────────────────────────────────── */

static void update_expression(struct pet_state *state, int64_t now_ms)
{
	int scores[PET_EXPR_COUNT];
	enum pet_expression best_expr;
	enum pet_expression curr_expr;
	int best_score;
	int curr_score;
	int expr;
	int32_t hold_ms;
	int margin;

	/* Context-sensitive petting window: drowsy petting shows COZY
	 * and lingers a bit longer than the default HAPPY. */
	const bool drowsy_pet = (state->sleepiness > 60) && (state->arousal < 35);
	const int32_t pet_window_ms = drowsy_pet ? (8 * MSEC_PER_SEC) : (6 * MSEC_PER_SEC);
	const bool pet_recent = (now_ms - state->last_pet_timestamp_ms) < pet_window_ms;

	/* Dynamic hold: influenced by intent stability.
	 *   High arousal / just picked up  → short hold, fast reactions
	 *   Strong intent                  → long hold, stable leaning
	 *   Moderate arousal               → medium hold
	 *   Low arousal                    → long hold, stable calm */
	if ((state->arousal >= 50) || state->picked_up_recently) {
		hold_ms = 4 * MSEC_PER_SEC;
		margin = 4;
	} else if (g_runtime.intent_strength > 40) {
		hold_ms = 10 * MSEC_PER_SEC;
		margin = 10;
	} else if (state->arousal >= 30) {
		hold_ms = 7 * MSEC_PER_SEC;
		margin = 6;
	} else {
		hold_ms = 12 * MSEC_PER_SEC;
		margin = 8;
	}

	/* Forced expression override */
	if (g_runtime.forced_expression_active) {
		if (state->current_expression != g_runtime.forced_expression) {
			state->current_expression = g_runtime.forced_expression;
			state->last_expression_change_timestamp_ms = now_ms;
		}
		return;
	}

	/* Asleep override */
	if (state->current_mode == PET_MODE_ASLEEP) {
		if (state->current_expression != PET_EXPR_ASLEEP) {
			state->current_expression = PET_EXPR_ASLEEP;
			state->last_expression_change_timestamp_ms = now_ms;
		}
		return;
	}

	/* Recent petting override: contextual expression */
	if (pet_recent) {
		enum pet_expression pet_expr = drowsy_pet ? PET_EXPR_COZY : PET_EXPR_HAPPY;

		if (state->current_expression != pet_expr) {
			state->current_expression = pet_expr;
			state->last_expression_change_timestamp_ms = now_ms;
		}
		return;
	}

	/* ── Score all expressions ── */
	curr_expr = state->current_expression;

	for (expr = 0; expr < PET_EXPR_COUNT; expr++) {
		scores[expr] = score_for_expression((enum pet_expression)expr, state, now_ms);
		scores[expr] += transition_affinity(curr_expr, (enum pet_expression)expr);
	}

	best_expr = PET_EXPR_CALM;
	best_score = scores[PET_EXPR_CALM];

	for (expr = 1; expr < PET_EXPR_COUNT; expr++) {
		if (scores[expr] > best_score) {
			best_expr = (enum pet_expression)expr;
			best_score = scores[expr];
		}
	}

	curr_score = scores[curr_expr];

	/* Hysteresis: require margin to switch */
	if (best_expr != curr_expr) {
		if ((now_ms - state->last_expression_change_timestamp_ms) < hold_ms &&
		    best_score < (curr_score + margin)) {
			return;
		}

		if (best_score < (curr_score + (margin - 2))) {
			return;
		}

		state->current_expression = best_expr;
		state->last_expression_change_timestamp_ms = now_ms;
	}
}

/* ── Init ────────────────────────────────────────────────────────── */

void behavior_engine_init(struct pet_state *state, int64_t now_ms)
{
	const int64_t stale_recent_event_ms = now_ms - (60 * MSEC_PER_SEC);
	const int64_t stale_pet_ms = now_ms - (11 * 60 * MSEC_PER_SEC);

	(void)memset(state, 0, sizeof(*state));
	(void)memset(&g_runtime, 0, sizeof(g_runtime));
	g_runtime.forced_expression = PET_EXPR_CALM;
	g_runtime.forced_expression_active = false;

	/* Context starts at zero (no recent history) */
	g_runtime.ctx.stimulation = 0;
	g_runtime.ctx.comfort = 0;
	g_runtime.ctx.social_warmth = 0;

	/* Intent starts at NONE */
	g_runtime.intent = PET_INTENT_NONE;
	g_runtime.intent_strength = 0;
	g_runtime.intent_resolved_at_ms = now_ms;

	/* Core emotional state */
	state->energy = 72;
	state->sleepiness = 22;
	state->attachment = 50;
	state->boredom = 20;
	state->stress = 18;
	state->arousal = 30;
	state->social_load = 10;
	state->trust = 55;
	state->curiosity = 45;
	state->walk_confidence = 0;
	state->walking_confidence = 0;
	state->notification_burst_level = 0;

	/* Seed boot in a neutral state */
	state->last_pet_timestamp_ms = stale_pet_ms;
	state->last_real_interaction_timestamp_ms = stale_recent_event_ms;
	state->last_motion_timestamp_ms = stale_recent_event_ms;
	state->last_walk_timestamp_ms = stale_recent_event_ms;
	state->last_phone_event_timestamp_ms = stale_recent_event_ms;
	state->last_self_wake_timestamp_ms = stale_recent_event_ms;
	state->last_reaction_timestamp_ms = now_ms;
	state->last_rough_event_timestamp_ms = now_ms - (2 * 60 * MSEC_PER_SEC);
	state->last_display_state_change_ms = now_ms;
	state->last_expression_change_timestamp_ms = now_ms;

	state->current_mode = PET_MODE_IDLE;
	state->current_expression = PET_EXPR_CALM;
	state->current_reaction = REACTION_NONE;
	state->current_display_state = DISPLAY_FOREGROUND;
	state->current_indicator = -1;
	state->current_overlay = -1;
	state->look_target_x = 0;
	state->look_target_y = 0;
	state->look_render_x = 0;
	state->look_render_y = 0;
	state->look_confidence = 0;
	state->in_hand = false;
	state->pickup_confidence = 0;
	state->in_hand_confidence = 0;
	state->picked_up_recently = false;
	state->battery_percent = -1;
	state->battery_percent_known = false;
	state->walking_session_start_ms = 0;
	state->last_hw_step_counter = 0U;
	state->walking_active = false;
	state->last_pickup_timestamp_ms = stale_recent_event_ms;
	state->last_in_hand_timestamp_ms = stale_recent_event_ms;
	state->last_motion_sample_timestamp_ms = stale_recent_event_ms;
	state->last_still_timestamp_ms = stale_recent_event_ms;
	state->last_motion_wake_reaction_ms = stale_recent_event_ms;
	state->dynamic_pupils_forced_disabled = false;

	state->ambient_wake_enabled = true;
	state->time_valid = false;
	state->tz_offset_minutes = 0;
	state->unix_time_at_sync = 0;
	state->uptime_at_sync_ms = now_ms;

	micro_reaction_init(now_ms);
}

/* ── Main pipeline ───────────────────────────────────────────────── */

void behavior_engine_handle_event(struct pet_state *state, const struct app_event *event)
{
	const enum pet_mode prev_mode = state->current_mode;
	const enum pet_expression prev_expr = state->current_expression;

	/* 1-3. Interpret event, apply immediate reaction, update state */
	apply_event(state, event);

	/* 4. Clamp state + context */
	clamp_state(state);
	clamp_context();

	/* 5. Resolve intent (throttled to every 5 s) */
	resolve_intent(state, event->timestamp_ms);

	/* 6. Resolve mode */
	update_mode(state, event->timestamp_ms);

	/* 7. Resolve expression (with hysteresis) */
	update_expression(state, event->timestamp_ms);

	/* Sync reaction state */
	state->current_reaction = micro_reaction_get_active(event->timestamp_ms);
	state->uptime_seconds = event->timestamp_ms / MSEC_PER_SEC;

	/* Log transitions */
	if (prev_mode != state->current_mode) {
		LOG_INF("Mode: %s -> %s (event=%s)", pet_mode_str(prev_mode),
			pet_mode_str(state->current_mode), app_event_type_str(event->type));
	}

	if (prev_expr != state->current_expression) {
		LOG_INF("Expr: %s -> %s (event=%s)", pet_expression_str(prev_expr),
			pet_expression_str(state->current_expression),
			app_event_type_str(event->type));
	}
}

/* ── String helpers ──────────────────────────────────────────────── */

const char *pet_mode_str(enum pet_mode mode)
{
	switch (mode) {
	case PET_MODE_ASLEEP:      return "ASLEEP";
	case PET_MODE_DROWSY:      return "DROWSY";
	case PET_MODE_IDLE:        return "IDLE";
	case PET_MODE_INTERACTING: return "INTERACTING";
	case PET_MODE_WALK_AWAKE:  return "WALK_AWAKE";
	case PET_MODE_TASK_ALERT:  return "TASK_ALERT";
	case PET_MODE_CHARGING:    return "CHARGING";
	case PET_MODE_LOW_POWER:   return "LOW_POWER";
	case PET_MODE_OVERLOADED:  return "OVERLOADED";
	default:                   return "UNKNOWN";
	}
}

const char *pet_expression_str(enum pet_expression expression)
{
	switch (expression) {
	case PET_EXPR_CALM:           return "CALM";
	case PET_EXPR_CURIOUS:        return "CURIOUS";
	case PET_EXPR_CONTENT:        return "CONTENT";
	case PET_EXPR_HAPPY:          return "HAPPY";
	case PET_EXPR_PLAYFUL:        return "PLAYFUL";
	case PET_EXPR_SLEEPY:         return "SLEEPY";
	case PET_EXPR_NEEDY:          return "NEEDY";
	case PET_EXPR_LONELY:         return "LONELY";
	case PET_EXPR_ANNOYED:        return "ANNOYED";
	case PET_EXPR_OVERSTIMULATED: return "OVERSTIMULATED";
	case PET_EXPR_COZY:           return "COZY";
	case PET_EXPR_DRAINED:        return "DRAINED";
	case PET_EXPR_ASLEEP:         return "ASLEEP";
	default:                      return "UNKNOWN";
	}
}

const char *pet_display_state_str(enum pet_display_state state)
{
	switch (state) {
	case DISPLAY_FOREGROUND: return "FOREGROUND";
	case DISPLAY_AMBIENT:    return "AMBIENT";
	case DISPLAY_OFF:        return "OFF";
	default:                 return "UNKNOWN";
	}
}

void behavior_engine_status_dump(const struct pet_state *state, char *buffer, size_t buffer_len)
{
	if ((buffer == NULL) || (buffer_len == 0U)) {
		return;
	}

	(void)snprintf(buffer, buffer_len,
		"mode=%s expr=%s force=%s disp=%s react=%s ind=%d ov=%d look_t=%d,%d "
		"look_r=%d,%d look_c=%u carry=%d/%u/%u/%u walk_act=%d step_day=%u "
		"total=%u hw=%u batt=%d known=%d E=%d Sl=%d Bo=%d St=%d Tr=%d Cu=%d "
		"At=%d ctx=%d/%d/%d intent=%s(%d) "
		"last_m=%lld last_w=%lld last_p=%lld last_h=%lld last_s=%lld "
		"dyn=%d time=%s",
		       pet_mode_str(state->current_mode),
		       pet_expression_str(state->current_expression),
		       g_runtime.forced_expression_active ?
			       pet_expression_str(g_runtime.forced_expression) : "AUTO",
		       pet_display_state_str(state->current_display_state),
		       micro_reaction_str(state->current_reaction),
		       state->current_indicator,
		       state->current_overlay,
		       state->look_target_x,
		       state->look_target_y,
		       state->look_render_x,
		       state->look_render_y,
		       state->look_confidence,
		       state->in_hand ? 1 : 0,
		       state->pickup_confidence,
		       state->in_hand_confidence,
		       state->walking_confidence,
		       state->walking_active ? 1 : 0,
		       state->step_count_today,
		       state->total_steps_since_boot,
		       state->last_hw_step_counter,
		       state->battery_percent,
		       state->battery_percent_known ? 1 : 0,
		       state->energy, state->sleepiness, state->boredom, state->stress,
		       state->trust, state->curiosity, state->attachment,
		       g_runtime.ctx.stimulation, g_runtime.ctx.comfort,
		       g_runtime.ctx.social_warmth,
		       pet_intent_str(g_runtime.intent), g_runtime.intent_strength,
		       (long long)state->last_motion_timestamp_ms,
		       (long long)state->last_walk_timestamp_ms,
		       (long long)state->last_pickup_timestamp_ms,
		       (long long)state->last_in_hand_timestamp_ms,
		       (long long)state->last_still_timestamp_ms,
		       state->dynamic_pupils_forced_disabled ? 1 : 0,
		       state->time_valid ? "valid" : "invalid");
}
