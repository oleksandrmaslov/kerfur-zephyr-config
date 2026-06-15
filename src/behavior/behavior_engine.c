/*
 * Behavior engine — the emotional core ("mini AI").
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
 * Emotional layers, fastest to slowest:
 *   micro-reactions (seconds) → afterglow context (a minute) →
 *   drives (minutes) → mood (hours) → persisted traits
 *   (attachment / trust / mood / personality survive reboots).
 *
 * Design goals:
 *   - Same event feels different depending on current emotional state
 *   - Short-lived context creates emotional momentum / afterglow
 *   - Intent gives the pet a brief behavioral "leaning"
 *   - Mood gives the pet a day-scale temperament
 *   - Personality biases gains/decays (same events, different pet)
 *   - Peer encounters depend on how the *other* Kerfur feels (contagion)
 *   - Expression scoring integrates context + intent for stability
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include "behavior/appraisal.h"
#include "behavior/behavior_engine.h"
#include "behavior/emotion_memory.h"
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

	/* Expression transition routing: appraisal + hysteresis choose a
	 * stable target feeling; the displayed expression then walks toward it
	 * one adjacency hop at a time so emotionally-distant changes ease
	 * through intermediates (e.g. ANGRY -> CALM -> HAPPY, ASLEEP ->
	 * SLEEPY -> ...) instead of snapping. */
	enum pet_expression expr_target;
	int64_t last_target_change_ms;
	int64_t last_expr_hop_ms;

	/* Mood: experience lands here first; the 60 s tick converts it
	 * into at most ±2 mood per minute (rate limit against spam). */
	int16_t mood_accum;

	/* Personality */
	enum pet_personality personality;
	uint8_t rest_accum_pct;        /* fractional sleepiness accumulation */

	/* Sleep inertia: pet stays visibly drowsy for a while after a
	 * deep-sleep wake-up. */
	int64_t groggy_until_ms;

	/* Idle micro-life scheduling */
	int64_t next_idle_quirk_ms;
	bool idle_quirk_flip;

	/* Emotional memory bookkeeping */
	struct emotion_memory last_saved_memory;
	int64_t last_memory_save_ms;
	uint32_t lifetime_pets;
	bool worn_style_loaded;
};

static struct behavior_runtime g_runtime;

/* Defined with the scoring layer below; used by gating helpers above it. */
static enum pet_situation resolve_situation(const struct pet_state *state);

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
	state->mood = clamp_0_100(state->mood);
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

/* ── Personality ─────────────────────────────────────────────────── *
 *
 * Personality is not a label: it scales how strongly classes of
 * experience land. Same events, different pet. IDs are persisted in
 * emotional memory — append new profiles, never reorder.
 */

struct personality_profile {
	const char *name;
	uint8_t warmth_pct;  /* touch → attachment/trust/comfort gains */
	uint8_t stress_pct;  /* stress increments from rough/overload */
	uint8_t social_pct;  /* peer + notification social/curiosity gains */
	uint8_t play_pct;    /* arousal gains from play, PLAY intent score */
	uint8_t rest_pct;    /* passive sleepiness accumulation */
};

static const struct personality_profile g_personalities[PET_PERSONALITY_COUNT] = {
	[PET_PERSONALITY_BALANCED] = { "balanced", 100, 100, 100, 100, 100 },
	[PET_PERSONALITY_CURIOUS]  = { "curious",  100,  95, 120, 105,  90 },
	[PET_PERSONALITY_SHY]      = { "shy",      115, 125,  70,  85, 105 },
	[PET_PERSONALITY_PLAYFUL]  = { "playful",  100,  90, 110, 130,  85 },
	[PET_PERSONALITY_CALM]     = { "calm",     105,  70,  95,  85, 115 },
};

static const struct personality_profile *personality(void)
{
	return &g_personalities[g_runtime.personality];
}

/* Scale a positive emotional increment by a personality percentage
 * (rounded). Only meant for positive values. */
static int scale_pct(int value, uint8_t pct)
{
	return ((value * (int)pct) + 50) / 100;
}

/* ── Mood (slow valence) ─────────────────────────────────────────── */

#define MOOD_ACCUM_LIMIT 60

static void nudge_mood(int delta)
{
	g_runtime.mood_accum = (int16_t)CLAMP(g_runtime.mood_accum + delta,
					      -MOOD_ACCUM_LIMIT, MOOD_ACCUM_LIMIT);
}

static int mood_bias(const struct pet_state *state)
{
	return (state->mood - 50) / 6; /* -8..+8 */
}

/* ── Sleep inertia ───────────────────────────────────────────────── */

static bool is_groggy(int64_t now_ms)
{
	return now_ms < g_runtime.groggy_until_ms;
}

static void trigger_reaction(struct pet_state *state, enum micro_reaction_type reaction, int64_t now_ms)
{
	/* Sleep inertia: a just-woken pet does not bounce. */
	if ((reaction == REACTION_HAPPY_BOUNCE) && is_groggy(now_ms)) {
		reaction = REACTION_BLINK;
	}

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

/* ── Emotional memory (persistence of slow traits) ───────────────── */

#define EMOTION_SAVE_MIN_INTERVAL_MS (25LL * 60 * MSEC_PER_SEC)

static int trait_delta(int16_t a, int16_t b)
{
	int d = (int)a - (int)b;

	return (d < 0) ? -d : d;
}

static void fill_memory_record(const struct pet_state *state, struct emotion_memory *out)
{
	out->personality = (uint8_t)g_runtime.personality;
	out->attachment = state->attachment;
	out->trust = state->trust;
	out->mood = state->mood;
	out->lifetime_pets = g_runtime.lifetime_pets;
	out->worn_expressive = state->worn_expressive;
}

static bool memory_changed_enough(const struct emotion_memory *cur,
				  const struct emotion_memory *saved)
{
	return (trait_delta(cur->attachment, saved->attachment) >= 3) ||
	       (trait_delta(cur->trust, saved->trust) >= 3) ||
	       (trait_delta(cur->mood, saved->mood) >= 4) ||
	       (cur->personality != saved->personality) ||
	       (cur->worn_expressive != saved->worn_expressive) ||
	       ((cur->lifetime_pets - saved->lifetime_pets) >= 10U);
}

/* Persist slow traits if they drifted meaningfully and the flash-wear
 * throttle allows it. min_interval_ms == 0 forces past the throttle
 * (used for explicit personality changes). */
static void try_save_emotion_memory(const struct pet_state *state, int64_t now_ms,
				    int64_t min_interval_ms)
{
	struct emotion_memory cur;

	if (!IS_ENABLED(CONFIG_KERFUR_EMOTION_MEMORY)) {
		return;
	}

	if ((now_ms - g_runtime.last_memory_save_ms) < min_interval_ms) {
		return;
	}

	fill_memory_record(state, &cur);
	if (!memory_changed_enough(&cur, &g_runtime.last_saved_memory)) {
		return;
	}

	if (emotion_memory_store(&cur) == 0) {
		g_runtime.last_saved_memory = cur;
		g_runtime.last_memory_save_ms = now_ms;
	}
}

/* ── Idle micro-life ─────────────────────────────────────────────── *
 *
 * When the world is quiet and the screen is on, the pet occasionally
 * does something small on its own: glances around when curious, peeks
 * up when it wants attention, slow-blinks when resting. One short
 * micro-reaction every ~25–70 s (slower in ambient / drowsy), never
 * when battery is low — alive, not fidgety.
 */

static void schedule_idle_quirk(const struct pet_state *state, int64_t now_ms)
{
	int64_t delay_ms = (25LL * MSEC_PER_SEC) +
			   (int64_t)(sys_rand32_get() % (45U * MSEC_PER_SEC));

	if (state->current_display_state == DISPLAY_AMBIENT) {
		delay_ms *= 2;
	}
	if (state->current_mode == PET_MODE_DROWSY) {
		delay_ms += delay_ms / 2;
	}

	g_runtime.next_idle_quirk_ms = now_ms + delay_ms;
}

static bool situation_allows_idle_quirks(const struct pet_state *state)
{
	switch (resolve_situation(state)) {
	case PET_SITUATION_WORN_QUIET: /* dark screen on a bag strap */
	case PET_SITUATION_SOCIAL:     /* busy with another Kerfur */
		return false;
	default:
		return true;
	}
}

static void maybe_idle_quirk(struct pet_state *state, int64_t now_ms)
{
	enum micro_reaction_type pick;

	if (g_runtime.next_idle_quirk_ms == 0LL) {
		schedule_idle_quirk(state, now_ms);
		return;
	}
	if (now_ms < g_runtime.next_idle_quirk_ms) {
		return;
	}

	/* Only when nothing else is going on and the face is visible. */
	if ((state->current_display_state == DISPLAY_OFF) ||
	    state->battery_low || state->battery_critical ||
	    state->in_hand || state->walking_active ||
	    (state->current_reaction != REACTION_NONE) ||
	    (g_runtime.ctx.stimulation >= 12) ||
	    !situation_allows_idle_quirks(state) ||
	    ((state->current_mode != PET_MODE_IDLE) &&
	     (state->current_mode != PET_MODE_DROWSY) &&
	     (state->current_mode != PET_MODE_WORN))) {
		schedule_idle_quirk(state, now_ms);
		return;
	}

	if ((g_runtime.intent == PET_INTENT_SEEK_ATTENTION) || (state->boredom > 55)) {
		pick = REACTION_LOOK_UP; /* checking for its human */
	} else if ((g_runtime.intent == PET_INTENT_OBSERVE) || (state->curiosity > 55)) {
		pick = g_runtime.idle_quirk_flip ? REACTION_GLANCE_RIGHT
						 : REACTION_GLANCE_LEFT;
	} else if ((g_runtime.intent == PET_INTENT_REST) || (state->sleepiness > 60)) {
		pick = REACTION_BLINK;
	} else {
		pick = g_runtime.idle_quirk_flip ? REACTION_GLANCE_LEFT : REACTION_BLINK;
	}
	g_runtime.idle_quirk_flip = !g_runtime.idle_quirk_flip;

	trigger_reaction(state, pick, now_ms);
	schedule_idle_quirk(state, now_ms);
}

/* ── Peer emotional contagion ────────────────────────────────────── *
 *
 * The nearby beacon carries the other Kerfur's mode/expression summary.
 * Meeting another Kerfur depends on how that Kerfur feels: tip-toe
 * around a resting one, light up with a bright one, show concern for a
 * strained or lonely one. "First Kerfus meet each other."
 */

enum peer_vibe {
	PEER_VIBE_NEUTRAL = 0,
	PEER_VIBE_RESTING,
	PEER_VIBE_BRIGHT,
	PEER_VIBE_STRAINED,
};

static enum peer_vibe peer_vibe_from_payload(const struct app_event_peer *peer)
{
	const uint8_t mode = peer->mode_summary;
	const uint8_t expr = peer->expression_summary;

	if ((mode == (uint8_t)PET_MODE_ASLEEP) || (mode == (uint8_t)PET_MODE_DROWSY) ||
	    (mode == (uint8_t)PET_MODE_LOW_POWER) || (mode == (uint8_t)PET_MODE_CHARGING) ||
	    (expr == (uint8_t)PET_EXPR_ASLEEP) || (expr == (uint8_t)PET_EXPR_SLEEPY) ||
	    (expr == (uint8_t)PET_EXPR_DRAINED) || (expr == (uint8_t)PET_EXPR_COZY)) {
		return PEER_VIBE_RESTING;
	}
	if ((mode == (uint8_t)PET_MODE_OVERLOADED) ||
	    (expr == (uint8_t)PET_EXPR_ANNOYED) ||
	    (expr == (uint8_t)PET_EXPR_OVERSTIMULATED) ||
	    (expr == (uint8_t)PET_EXPR_NEEDY) || (expr == (uint8_t)PET_EXPR_LONELY)) {
		return PEER_VIBE_STRAINED;
	}
	if ((expr == (uint8_t)PET_EXPR_HAPPY) || (expr == (uint8_t)PET_EXPR_PLAYFUL) ||
	    (expr == (uint8_t)PET_EXPR_CONTENT)) {
		return PEER_VIBE_BRIGHT;
	}
	return PEER_VIBE_NEUTRAL;
}

static const char *peer_vibe_str(enum peer_vibe vibe)
{
	switch (vibe) {
	case PEER_VIBE_RESTING:  return "RESTING";
	case PEER_VIBE_BRIGHT:   return "BRIGHT";
	case PEER_VIBE_STRAINED: return "STRAINED";
	default:                 return "NEUTRAL";
	}
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
			(g_runtime.ctx.comfort > 30 ? 8 : 0) +
			(is_groggy(now_ms) ? 12 : 0));
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

	/* PLAY: energetic + trusting + not stressed.
	 * Personality scales the appetite; mood tilts it. */
	if (state->arousal > 45 && state->trust > 35 && state->stress < 45) {
		scores[PET_INTENT_PLAY] = (int16_t)(
			scale_pct(state->arousal / 2 +
				  state->trust / 5 -
				  state->stress / 4 +
				  (state->energy > 40 ? 5 : 0),
				  personality()->play_pct) +
			mood_bias(state));
	}

	/* SELF_SOOTHE: moderate stress + recent comfort */
	if (state->stress > 25 && g_runtime.ctx.comfort > 15) {
		scores[PET_INTENT_SELF_SOOTHE] = (int16_t)(
			state->stress / 3 +
			g_runtime.ctx.comfort / 3);
	}

	/* WITHDRAW: high stress or overstimulated; a good mood buffers it */
	if (state->stress > 45 || g_runtime.ctx.stimulation > 55) {
		scores[PET_INTENT_WITHDRAW] = (int16_t)(
			state->stress / 2 +
			(g_runtime.ctx.stimulation > 55
				? (g_runtime.ctx.stimulation - 35) / 3 : 0) -
			(g_runtime.ctx.comfort > 30 ? 8 : 0) -
			mood_bias(state) / 2);
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

	/* Mood drifts home to neutral when no fresh experience is pending. */
	if (g_runtime.mood_accum == 0) {
		if (state->mood > 50) {
			state->mood -= 1;
		} else if (state->mood < 50) {
			state->mood += 1;
		}
	}
}

static void apply_30min_drift(struct pet_state *state, int64_t now_ms)
{
	const int64_t no_interaction_ms = now_ms - state->last_real_interaction_timestamp_ms;
	const int64_t no_rough_ms = now_ms - state->last_rough_event_timestamp_ms;

	if (no_interaction_ms >= (2LL * 60 * 60 * MSEC_PER_SEC)) {
		state->attachment -= 1;
		/* Long loneliness wears on the mood, too. */
		nudge_mood(-2);
	}

	if (no_rough_ms >= (2LL * 60 * 60 * MSEC_PER_SEC)) {
		state->trust += 1;
	}

	try_save_emotion_memory(state, now_ms, EMOTION_SAVE_MIN_INTERVAL_MS);
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
			/* Shared walks are good for the soul. */
			nudge_mood(1);
			trigger_reaction(state,
					 (now_ms & 1LL) ? REACTION_GLANCE_RIGHT
							: REACTION_GLANCE_LEFT,
					 now_ms);
		}
	}

	/* Context afterglow decay */
	decay_context();

	/* Social overload: emerges from social_load accumulating over time, not
	 * from a raw peer headcount.  Multiple simultaneous encounters drive
	 * social_load up via ENCOUNTER_START (+4 each); it decays in the 10 s
	 * tick.  The flag propagates to the beacon so nearby peers know we are
	 * saturated and can choose not to initiate new encounters. */
	state->social_overload = (state->social_load > 55) && (state->stress > 30);

	/* Autonomous idle micro-life */
	maybe_idle_quirk(state, now_ms);

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
		/* Personality-scaled tiredness: rest_pct=100 -> +1/min. */
		g_runtime.rest_accum_pct += personality()->rest_pct;
		while (g_runtime.rest_accum_pct >= 100U) {
			g_runtime.rest_accum_pct -= 100U;
			state->sleepiness += 1;
		}
	} else {
		state->sleepiness -= 1;
	}

	/* Convert accumulated experience into mood (max ±2/min); stale
	 * residue evaporates instead of acting minutes later. */
	{
		const int step = CLAMP(g_runtime.mood_accum / 5, -2, 2);

		if (step != 0) {
			state->mood += step;
			g_runtime.mood_accum -= (int16_t)(step * 5);
		} else if (g_runtime.mood_accum > 0) {
			g_runtime.mood_accum--;
		} else if (g_runtime.mood_accum < 0) {
			g_runtime.mood_accum++;
		}
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
		nudge_mood(1);

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

		g_runtime.ctx.comfort += scale_pct(20, personality()->warmth_pct);
		g_runtime.ctx.stimulation += 6;
		state->last_pet_timestamp_ms = now;
		g_runtime.lifetime_pets++;
		nudge_mood(4);

		/* Base warmth and trust (personality-scaled) */
		state->attachment += scale_pct(8, personality()->warmth_pct);
		state->trust += scale_pct(6, personality()->warmth_pct);
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

		g_runtime.ctx.comfort += scale_pct(25, personality()->warmth_pct);
		g_runtime.ctx.stimulation += 5;
		state->last_pet_timestamp_ms = now;
		g_runtime.lifetime_pets++;
		nudge_mood(5);

		state->attachment += scale_pct(10, personality()->warmth_pct);
		state->trust += scale_pct(4, personality()->warmth_pct);
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
		g_runtime.ctx.comfort += scale_pct(15, personality()->warmth_pct);
		g_runtime.ctx.stimulation += 3;
		state->last_pet_timestamp_ms = now;
		nudge_mood(2);

		state->attachment += scale_pct(4, personality()->warmth_pct);
		state->trust += scale_pct(3, personality()->warmth_pct);
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
			state->arousal += scale_pct(4, personality()->play_pct);
			state->curiosity += 2;
			state->boredom -= 2;
			state->sleepiness -= 3;
			state->stress += (state->trust >= 40) ?
					 0 : scale_pct(2, personality()->stress_pct);

			if (playful && state->trust > 40) {
				state->boredom -= 2;
				nudge_mood(2);
				trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
			} else if (stressed_already) {
				trigger_reaction(state, REACTION_STARTLE, now);
			}
		} else {
			/* Play shake on surface: more startling */
			g_runtime.ctx.stimulation += 10;
			state->arousal += scale_pct(5, personality()->play_pct);
			state->curiosity += 3;
			state->boredom -= 3;
			state->sleepiness -= 4;
			state->stress += (state->trust >= 40) ?
					 1 : scale_pct(3, personality()->stress_pct);

			if (playful && state->trust > 40) {
				state->boredom -= 2;
				nudge_mood(1);
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
			state->stress += scale_pct(5, personality()->stress_pct);
			state->trust -= 2;
			state->attachment -= 1;
			nudge_mood(-8);
		} else {
			state->stress += scale_pct(10, personality()->stress_pct);
			state->trust -= 4;
			state->attachment -= 2;
			nudge_mood(-10);
		}

		trigger_reaction(state, REACTION_STARTLE, now);
		break;
	}

	case APP_EVENT_IMPACT: {
		g_runtime.ctx.stimulation += 15;
		g_runtime.ctx.comfort -= 4;

		state->stress += scale_pct(6, personality()->stress_pct);
		state->trust -= 1;
		nudge_mood(-6);
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
			nudge_mood(2);
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
		state->curiosity += scale_pct(6, personality()->social_pct);
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
			state->stress += scale_pct(12, personality()->stress_pct);
			state->curiosity -= 2;
			nudge_mood(-4);
		} else {
			/* First burst: stressful but manageable */
			state->stress += scale_pct(8, personality()->stress_pct);
			state->curiosity += 2;
			nudge_mood(-1);
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
		nudge_mood(2);

		/* Being tucked in while drowsy is extra cozy. */
		if (state->sleepiness > 60) {
			g_runtime.ctx.comfort += 4;
		}

		/* Docking is a natural moment to remember who we've become. */
		try_save_emotion_memory(state, now, 10LL * 60 * MSEC_PER_SEC);

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
		const bool is_familiar = !is_friend &&
					 (event->payload.peer.session_encounters > 0U);
		const enum peer_vibe vibe = peer_vibe_from_payload(&event->payload.peer);

		state->peer_nearby = true;
		state->current_active_peer_id = event->payload.peer.ephemeral_id;
		state->current_active_friend_index = event->payload.peer.friend_index;
		state->peer_known_friend = is_friend;
		state->social_load += 1;

		if (is_friend) {
			g_runtime.ctx.social_warmth += scale_pct(25, personality()->social_pct);
			g_runtime.ctx.comfort += 10;
			state->curiosity += scale_pct(5, personality()->social_pct);
			state->attachment += 2;
			nudge_mood(3);
		} else if (is_familiar) {
			/* Met this peer earlier in the session — warmer than a
			 * stranger, less novel than a friend.  More comfort, less
			 * raw stimulation. */
			g_runtime.ctx.social_warmth += scale_pct(18, personality()->social_pct);
			g_runtime.ctx.comfort += 4;
			state->curiosity += scale_pct(3, personality()->social_pct);
			state->attachment += 1;
			nudge_mood(2);
		} else {
			/* Fresh stranger — curious, stimulating. */
			g_runtime.ctx.social_warmth += scale_pct(15, personality()->social_pct);
			g_runtime.ctx.stimulation += 5;
			state->curiosity += scale_pct(6, personality()->social_pct);
			nudge_mood(2);
		}

		/* Contagion: how the other Kerfur feels colors the moment. */
		switch (vibe) {
		case PEER_VIBE_RESTING:
			/* Quiet interest — don't get worked up near a sleeper. */
			state->arousal -= 1;
			g_runtime.ctx.stimulation -= 3;
			break;
		case PEER_VIBE_BRIGHT:
			state->arousal += scale_pct(2, personality()->play_pct);
			nudge_mood(2);
			break;
		case PEER_VIBE_STRAINED:
			/* Mild concern for the other one. */
			state->stress += scale_pct(1, personality()->stress_pct);
			state->curiosity += 2;
			break;
		default:
			break;
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

	case APP_EVENT_PEER_LOST: {
		/* Use friend_index (stable across ID rotation) when available;
		 * fall back to ephemeral_id for unknown peers. */
		const int8_t lost_fidx = event->payload.peer.friend_index;
		const bool is_active_peer =
			(lost_fidx >= 0)
				? (state->current_active_friend_index == lost_fidx)
				: (state->current_active_peer_id ==
				   event->payload.peer.ephemeral_id);

		g_runtime.ctx.social_warmth -= 10;

		if (is_active_peer) {
			state->peer_nearby = false;
			state->peer_known_friend = false;
			state->current_active_peer_id = 0U;
			state->current_active_friend_index = -1;
		}
		if (state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE ||
		    state->social_indicator == (int16_t)KERFUR_FACE_INDICATOR_ICON_HEART_FILLED) {
			state->social_indicator = 0;
			state->social_indicator_until_ms = 0;
		}
		break;
	}

	case APP_EVENT_ENCOUNTER_START: {
		const bool is_friend = event->payload.peer.is_friend;
		const bool is_familiar = !is_friend &&
					 (event->payload.peer.session_encounters > 0U);
		const enum peer_vibe vibe = peer_vibe_from_payload(&event->payload.peer);
		enum micro_reaction_type hello;

		state->peer_nearby = true;
		state->current_active_peer_id = event->payload.peer.ephemeral_id;
		state->current_active_friend_index = event->payload.peer.friend_index;
		state->peer_known_friend = is_friend;

		/* Social load accumulates per-encounter so sustained multi-device
		 * presence eventually produces genuine emotional saturation.
		 * Friends tax less — they energize rather than drain. */
		state->social_load += is_friend ? 2 : 4;

		if (is_friend) {
			g_runtime.ctx.social_warmth += scale_pct(30, personality()->social_pct);
			g_runtime.ctx.comfort += 15;
			g_runtime.ctx.stimulation += 8;

			state->attachment += 6;
			state->trust += 3;
			state->stress -= 2;
			nudge_mood(8);

			/* Reuniting when bored is extra warm. */
			if (state->boredom > 40) {
				state->boredom -= 8;
				state->attachment += 3;
			}

			hello = REACTION_HAPPY_BOUNCE;
			switch (vibe) {
			case PEER_VIBE_RESTING:
				state->sleepiness += 2;
				state->arousal -= 1;
				g_runtime.ctx.stimulation -= 6;
				hello = REACTION_PET_BOW;
				break;
			case PEER_VIBE_STRAINED:
				state->stress += scale_pct(2, personality()->stress_pct);
				state->curiosity += 4;
				hello = REACTION_GLANCE_LEFT;
				break;
			case PEER_VIBE_BRIGHT:
				state->arousal += scale_pct(4, personality()->play_pct);
				nudge_mood(4);
				break;
			default:
				break;
			}

			show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_FILLED,
					      now, 3000);
			trigger_reaction(state, hello, now);

		} else if (is_familiar) {
			/* Already met this Kerfur earlier this session: recognition
			 * warmth — comfortable, a little less novel than the first
			 * meeting, more trust than a total stranger. */
			g_runtime.ctx.social_warmth += scale_pct(24, personality()->social_pct);
			g_runtime.ctx.comfort += 8;
			g_runtime.ctx.stimulation += 6;

			state->attachment += 3;
			state->trust += 2;
			state->curiosity += scale_pct(3, personality()->social_pct);
			nudge_mood(5);

			hello = REACTION_PET_BOW;
			switch (vibe) {
			case PEER_VIBE_RESTING:
				state->arousal -= 1;
				g_runtime.ctx.stimulation -= 4;
				hello = REACTION_GLANCE_LEFT;
				break;
			case PEER_VIBE_BRIGHT:
				state->arousal += scale_pct(2, personality()->play_pct);
				nudge_mood(2);
				if (g_runtime.intent == PET_INTENT_PLAY) {
					hello = REACTION_HAPPY_BOUNCE;
				}
				break;
			case PEER_VIBE_STRAINED:
				state->stress += scale_pct(1, personality()->stress_pct);
				state->curiosity += 2;
				break;
			default:
				break;
			}

			show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
					      now, 2500);
			trigger_reaction(state, hello, now);

		} else {
			/* First encounter with this peer — curious, stimulating,
			 * cautiously open. */
			g_runtime.ctx.social_warmth += scale_pct(20, personality()->social_pct);
			g_runtime.ctx.stimulation += 10;

			state->curiosity += scale_pct(6, personality()->social_pct);
			state->attachment += 2;
			nudge_mood(3);

			if (g_runtime.intent == PET_INTENT_OBSERVE) {
				state->curiosity += 3;
			}

			hello = REACTION_PET_BOW;
			switch (vibe) {
			case PEER_VIBE_RESTING:
				state->arousal -= 1;
				g_runtime.ctx.stimulation -= 5;
				hello = REACTION_GLANCE_RIGHT;
				break;
			case PEER_VIBE_STRAINED:
				state->stress += scale_pct(1, personality()->stress_pct);
				state->curiosity += 2;
				break;
			case PEER_VIBE_BRIGHT:
				state->arousal += scale_pct(3, personality()->play_pct);
				nudge_mood(2);
				if ((g_runtime.intent == PET_INTENT_PLAY) &&
				    (state->trust > 50)) {
					hello = REACTION_HAPPY_BOUNCE;
				}
				break;
			default:
				break;
			}

			show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
					      now, 3000);
			trigger_reaction(state, hello, now);
		}

		LOG_INF("Encounter start: %s peer vibe=%s",
			is_friend ? "friend" : "unknown", peer_vibe_str(vibe));
		state->encounter_sync_pending = true;
		break;
	}

	case APP_EVENT_ENCOUNTER_END: {
		const int8_t end_fidx = event->payload.peer.friend_index;
		const bool is_friend_enc = event->payload.peer.is_friend;
		const bool is_active_enc =
			(end_fidx >= 0)
				? (state->current_active_friend_index == end_fidx)
				: (state->current_active_peer_id ==
				   event->payload.peer.ephemeral_id);

		if (is_friend_enc) {
			/* Friend is leaving: warm afterglow lingers.  Social
			 * warmth drops less, no boredom spike — saying goodbye
			 * to a friend is not the same as being left alone. */
			g_runtime.ctx.social_warmth -= 8;
			if (event->payload.peer.duration_s >= 60) {
				nudge_mood(4);  /* real visit */
			} else if (event->payload.peer.duration_s >= 20) {
				nudge_mood(2);
			}
			trigger_reaction(state, REACTION_GLANCE_LEFT, now);
		} else {
			/* Unknown / familiar peer departed. */
			g_runtime.ctx.social_warmth -= 15;
			if (event->payload.peer.duration_s >= 30) {
				state->boredom += 3;
			}
			if (event->payload.peer.duration_s >= 60) {
				nudge_mood(2);
			}
			trigger_reaction(state, REACTION_GLANCE_RIGHT, now);
		}

		if (is_active_enc) {
			state->peer_nearby = false;
			state->peer_known_friend = false;
			state->current_active_peer_id = 0U;
			state->current_active_friend_index = -1;
		}
		state->social_indicator = 0;
		state->social_indicator_until_ms = 0;
		break;
	}

	case APP_EVENT_PEER_PLAY_INVITE:
		g_runtime.ctx.stimulation += 10;
		g_runtime.ctx.social_warmth += scale_pct(12, personality()->social_pct);

		state->arousal += scale_pct(8, personality()->play_pct);
		state->boredom -= 4;
		state->curiosity += 2;
		nudge_mood(3);
		show_social_indicator(state, KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
				      now, 2500);
		trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		break;

	case APP_EVENT_PEER_PLAY_ACK:
		g_runtime.ctx.stimulation += 8;
		g_runtime.ctx.social_warmth += scale_pct(10, personality()->social_pct);

		state->arousal += scale_pct(6, personality()->play_pct);
		state->boredom -= 3;
		state->attachment += 2;
		nudge_mood(3);
		trigger_reaction(state, REACTION_HAPPY_BOUNCE, now);
		break;

	/* ── System / lifecycle events ───────────────────────────────── */

	case APP_EVENT_WAKE: {
		/* Night wake-ups are reluctant: less alert, still sleepy. */
		const bool night = is_night_time(state, now);

		g_runtime.ctx.stimulation += 10;
		state->sleepiness -= night ? 4 : 8;
		state->arousal += night ? 3 : 5;
		trigger_reaction(state, REACTION_WAKE_BLINK, now);
		break;
	}

	case APP_EVENT_SLEEP_REQUEST:
		state->arousal -= 6;
		trigger_reaction(state, REACTION_SLEEP_FADE, now);
		break;

	case APP_EVENT_IDLE_TIMEOUT:
		/* Nothing has happened for a while — settle. */
		state->arousal -= 1;
		break;

	case APP_EVENT_DISPLAY_AMBIENT_TIMEOUT:
		/* Screen going dark: the pet lets go of the moment. */
		state->arousal -= 2;
		state->stress -= 1;
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
		if (event->payload.carry_state.carry_context < (uint8_t)PET_CARRY_TRANSITION + 1U) {
			state->carry_context = (enum pet_carry_context)
				event->payload.carry_state.carry_context;
			state->carry_context_confidence =
				event->payload.carry_state.carry_context_confidence;
		}
		if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG_VERBOSE)) {
			LOG_INF("Face carry -> in_hand=%d pickup=%u in_hand_conf=%u walk_conf=%u ctx=%u",
				state->in_hand ? 1 : 0, state->pickup_confidence,
				state->in_hand_confidence, state->walking_confidence,
				event->payload.carry_state.carry_context);
		}
		break;

	case APP_EVENT_CARRY_CONTEXT_CHANGED: {
		const enum pet_carry_context prev_ctx = state->carry_context;
		const uint8_t raw_ctx = event->payload.carry_state.carry_context;
		enum pet_carry_context ctx;

		if (raw_ctx > (uint8_t)PET_CARRY_TRANSITION) {
			break;
		}
		ctx = (enum pet_carry_context)raw_ctx;
		state->carry_context = ctx;
		state->carry_context_confidence =
			event->payload.carry_state.carry_context_confidence;

		/* Context edges carry meaning of their own. */
		if ((ctx == PET_CARRY_WORN) && (prev_ctx != PET_CARRY_WORN)) {
			/* Clipped on and moving: going on an adventure. */
			state->curiosity += 2;
			g_runtime.ctx.stimulation += 3;
			nudge_mood(1);
			LOG_INF("Carry context: worn (conf=%u)",
				state->carry_context_confidence);
		} else if ((prev_ctx == PET_CARRY_WORN) &&
			   (ctx == PET_CARRY_IN_HAND)) {
			/* Grabbed off the bag — "you picked me!" */
			state->attachment += 2;
			state->arousal += 3;
			g_runtime.ctx.comfort += 6;
			nudge_mood(2);
			trigger_reaction(state, REACTION_WAKE_BLINK, now);
		} else if ((prev_ctx == PET_CARRY_WORN) &&
			   (ctx == PET_CARRY_ON_SURFACE)) {
			/* Taken off and set down: settle. */
			state->arousal -= 1;
		}
		break;
	}

	case APP_EVENT_WORN_STYLE_SET:
		state->worn_expressive = (event->param != 0);
		LOG_INF("Worn style -> %s",
			state->worn_expressive ? "expressive" : "quiet");
		/* Explicit owner choice: persist immediately. */
		try_save_emotion_memory(state, now, 0);
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

	case APP_EVENT_FACE_DEBUG_DUMP: {
		char emotion[256];

		log_face_snapshot(state, "Face dump");
		behavior_engine_emotion_dump(state, emotion, sizeof(emotion));
		LOG_INF("Emotion dump %s", emotion);
		break;
	}

	case APP_EVENT_PERSONALITY_SET:
		if ((event->param < 0) || (event->param >= PET_PERSONALITY_COUNT)) {
			LOG_WRN("Ignoring invalid personality id=%d", event->param);
			break;
		}
		if (g_runtime.personality != (enum pet_personality)event->param) {
			g_runtime.personality = (enum pet_personality)event->param;
			LOG_INF("Personality -> %s", personality()->name);
			/* Persist immediately: an explicit owner choice. */
			try_save_emotion_memory(state, now, 0);
		}
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
	} else if ((state->carry_context == PET_CARRY_WORN) &&
		   (idle_ms >= (10 * MSEC_PER_SEC))) {
		/* Dangling on jeans/backpack, not being interacted with:
		 * along for the ride. A nap (above) still beats it. */
		mode = PET_MODE_WORN;
	} else if (state->sleepiness > drowsy_threshold) {
		mode = PET_MODE_DROWSY;
	} else if (idle_ms < (45 * MSEC_PER_SEC)) {
		mode = PET_MODE_INTERACTING;
	} else if (state->battery_low) {
		mode = PET_MODE_LOW_POWER;
	}

	state->current_mode = mode;
}

/* ── Situation resolution + expression scoring ───────────────────── *
 *
 * The deterministic situation layer decides *where the pet is living*
 * (charging dock, owner's hand, dangling on a backpack, ...). The
 * normalized appraisal table (behavior/appraisal.c) then scores
 * expressions within that situation. The engine keeps intent alignment,
 * transition affinity and hysteresis on top.
 */

static enum pet_situation resolve_situation(const struct pet_state *state)
{
	if (state->charging) {
		return PET_SITUATION_CHARGING;
	}
	if (state->peer_nearby) {
		return PET_SITUATION_SOCIAL;
	}
	if (state->in_hand || (state->carry_context == PET_CARRY_IN_HAND)) {
		return PET_SITUATION_ENGAGED;
	}
	if (state->carry_context == PET_CARRY_WORN) {
		return state->worn_expressive ? PET_SITUATION_WORN_LIVELY
					      : PET_SITUATION_WORN_QUIET;
	}
	return PET_SITUATION_RESTING;
}

static int intent_alignment_bonus(enum pet_expression expr)
{
	const int16_t is = g_runtime.intent_strength;

	switch (g_runtime.intent) {
	case PET_INTENT_REST:
		if (expr == PET_EXPR_SLEEPY) {
			return is / 6;
		}
		if (expr == PET_EXPR_COZY) {
			return is / 7;
		}
		if (expr == PET_EXPR_CALM) {
			return is / 9;
		}
		break;
	case PET_INTENT_PLAY:
		if (expr == PET_EXPR_PLAYFUL) {
			return is / 5;
		}
		if (expr == PET_EXPR_HAPPY) {
			return is / 7;
		}
		break;
	case PET_INTENT_OBSERVE:
		if (expr == PET_EXPR_CURIOUS) {
			return is / 5;
		}
		if (expr == PET_EXPR_CALM) {
			return is / 9;
		}
		break;
	case PET_INTENT_SEEK_ATTENTION:
		if (expr == PET_EXPR_NEEDY) {
			return is / 6;
		}
		if (expr == PET_EXPR_LONELY) {
			return is / 8;
		}
		break;
	case PET_INTENT_WITHDRAW:
		if (expr == PET_EXPR_ANNOYED) {
			return is / 6;
		}
		if (expr == PET_EXPR_OVERSTIMULATED) {
			return is / 7;
		}
		break;
	case PET_INTENT_SELF_SOOTHE:
		if (expr == PET_EXPR_CONTENT) {
			return is / 6;
		}
		if (expr == PET_EXPR_CALM) {
			return is / 7;
		}
		break;
	default:
		break;
	}
	return 0;
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
		/* Burnout crash: an overstimulated pet that runs out of energy
		 * slumps straight into exhaustion instead of detouring through
		 * serene CALM (which read wrong mid-meltdown). One direction only
		 * — a drained pet is never pulled back up toward overstimulation. */
		if (candidate == PET_EXPR_DRAINED) {
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
	case PET_EXPR_ASLEEP:
		/* Only used for transition routing (waking): ASLEEP eases out
		 * through SLEEPY. While actually asleep the mode override owns
		 * the expression, so this never biases live scoring. */
		if (candidate == PET_EXPR_SLEEPY) {
			return 5;
		}
		break;
	default:
		break;
	}
	return 0;
}

/* Two expressions are adjacent if either direction has transition affinity
 * (the affinity table is the emotional-neighbour graph). CALM is the hub. */
static bool expr_adjacent(enum pet_expression a, enum pet_expression b)
{
	return (a == b) || (transition_affinity(a, b) > 0) ||
	       (transition_affinity(b, a) > 0);
}

/* BFS one hop from `from` toward `to` on the adjacency graph: returns the
 * next expression to show on the way to `to` (== `to` when already adjacent).
 * The graph is small and connected through CALM, so a path always exists;
 * CALM is the guaranteed fallback. This is what makes the face *ease*
 * between distant feelings instead of jumping. */
static enum pet_expression expr_first_hop(enum pet_expression from,
					  enum pet_expression to)
{
	enum pet_expression queue[PET_EXPR_COUNT];
	enum pet_expression first_hop[PET_EXPR_COUNT];
	bool seen[PET_EXPR_COUNT] = { false };
	int head = 0;
	int tail = 0;
	int n;

	if ((from == to) || expr_adjacent(from, to)) {
		return to;
	}

	seen[from] = true;
	for (n = 0; n < PET_EXPR_COUNT; n++) {
		enum pet_expression e = (enum pet_expression)n;

		if ((e != from) && expr_adjacent(from, e)) {
			seen[e] = true;
			first_hop[e] = e;
			queue[tail++] = e;
		}
	}

	while (head < tail) {
		enum pet_expression cur = queue[head++];

		if (cur == to) {
			return first_hop[cur];
		}
		for (n = 0; n < PET_EXPR_COUNT; n++) {
			enum pet_expression e = (enum pet_expression)n;

			if (!seen[e] && expr_adjacent(cur, e)) {
				seen[e] = true;
				first_hop[e] = first_hop[cur];
				queue[tail++] = e;
			}
		}
	}

	return PET_EXPR_CALM;
}

/* ── Expression resolution ───────────────────────────────────────── */

/* One adjacency hop per this interval, so distant feelings ease through
 * intermediates at a believable pace instead of snapping. The election runs
 * on the 100 ms tick, so this paces the visible walk (~2 hops ≈ 1.2 s). */
#define EXPR_ROUTE_HOP_MS 600

/* Walk the displayed expression one adjacency hop toward `target`. The
 * hysteresis layer owns *what* we want to feel; this owns *easing there*. */
static void route_expression_toward(struct pet_state *state,
				    enum pet_expression target, int64_t now_ms)
{
	g_runtime.expr_target = target;

	if (state->current_expression == target) {
		return;
	}
	if ((now_ms - g_runtime.last_expr_hop_ms) < EXPR_ROUTE_HOP_MS) {
		return;
	}

	state->current_expression =
		expr_first_hop(state->current_expression, target);
	state->last_expression_change_timestamp_ms = now_ms;
	g_runtime.last_expr_hop_ms = now_ms;
}

static void update_expression(struct pet_state *state, int64_t now_ms)
{
	int scores[PET_EXPR_COUNT];
	enum pet_expression best_expr;
	enum pet_expression target;
	int best_score;
	int target_score;
	int expr;
	int32_t hold_ms;
	int margin;

	/* Context-sensitive petting window: drowsy petting shows COZY
	 * and lingers a bit longer than the default HAPPY. */
	const bool drowsy_pet = (state->sleepiness > 60) && (state->arousal < 35);
	const int32_t pet_window_ms = drowsy_pet ? (8 * MSEC_PER_SEC) : (6 * MSEC_PER_SEC);
	const bool pet_recent = (now_ms - state->last_pet_timestamp_ms) < pet_window_ms;

	/* Dynamic hold: influenced by intent stability. Governs how readily the
	 * *target* feeling is allowed to change (anti-flap); the routing layer
	 * then eases the face toward whatever target wins.
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

	/* Forced expression override (debug): snap instantly. */
	if (g_runtime.forced_expression_active) {
		g_runtime.expr_target = g_runtime.forced_expression;
		if (state->current_expression != g_runtime.forced_expression) {
			state->current_expression = g_runtime.forced_expression;
			state->last_expression_change_timestamp_ms = now_ms;
		}
		g_runtime.last_expr_hop_ms = now_ms;
		return;
	}

	/* Asleep override: snap to ASLEEP while truly asleep (the eyes are
	 * shut). Waking then eases out, because the target switches to a live
	 * feeling while the face is still ASLEEP — routing walks
	 * ASLEEP → SLEEPY → … instead of jumping straight to awake. */
	if (state->current_mode == PET_MODE_ASLEEP) {
		g_runtime.expr_target = PET_EXPR_ASLEEP;
		if (state->current_expression != PET_EXPR_ASLEEP) {
			state->current_expression = PET_EXPR_ASLEEP;
			state->last_expression_change_timestamp_ms = now_ms;
		}
		g_runtime.last_expr_hop_ms = now_ms;
		return;
	}

	/* Recent petting: target the contextual pet expression, but EASE there.
	 * Petting an angry pet now soothes ANGRY → CALM → HAPPY rather than
	 * snapping straight to HAPPY (and back when the window ends). */
	if (pet_recent) {
		enum pet_expression pet_expr = drowsy_pet ? PET_EXPR_COZY : PET_EXPR_HAPPY;

		if (g_runtime.expr_target != pet_expr) {
			g_runtime.last_target_change_ms = now_ms;
		}
		route_expression_toward(state, pet_expr, now_ms);
		return;
	}

	/* ── Score all expressions (situation + normalized appraisal) ── */
	target = g_runtime.expr_target;

	{
		const enum pet_situation situation = resolve_situation(state);
		struct appraisal_inputs in = {
			.state = state,
			.ctx_stimulation = g_runtime.ctx.stimulation,
			.ctx_comfort = g_runtime.ctx.comfort,
			.ctx_social_warmth = g_runtime.ctx.social_warmth,
			.groggy = is_groggy(now_ms),
			.night = is_night_time(state, now_ms),
			.now_ms = now_ms,
		};
		uint8_t features[APF_COUNT];

		appraisal_compute_features(&in, features);

		for (expr = 0; expr < PET_EXPR_COUNT; expr++) {
			if (!appraisal_expression_allowed((enum pet_expression)expr,
							  situation)) {
				scores[expr] = -1000;
				continue;
			}
			scores[expr] = appraisal_expression_score(
					       (enum pet_expression)expr,
					       situation, features) +
				       intent_alignment_bonus((enum pet_expression)expr) +
				       transition_affinity(target,
							   (enum pet_expression)expr);
		}

		/* Incumbency on the committed target: affinity hands neighbours
		 * up to +5 while the target gets 0, which at the tight hold
		 * margin would make adjacent retargets cheaper than no
		 * hysteresis at all. This stickiness restores the balance;
		 * genuine shifts (tools/appraisal_calibrate.py) still win. */
		if (scores[target] > -1000) {
			scores[target] += 3;
		}
	}

	best_expr = PET_EXPR_CALM;
	best_score = scores[PET_EXPR_CALM];

	for (expr = 1; expr < PET_EXPR_COUNT; expr++) {
		if (scores[expr] > best_score) {
			best_expr = (enum pet_expression)expr;
			best_score = scores[expr];
		}
	}

	target_score = scores[target];

	/* Hysteresis decides whether to re-target; routing eases the face there.
	 * Inside the hold window a full margin is needed; after it, margin-2. */
	if (best_expr != target) {
		const bool held =
			(now_ms - g_runtime.last_target_change_ms) >= hold_ms;

		if (held ? (best_score >= (target_score + (margin - 2)))
			 : (best_score >= (target_score + margin))) {
			target = best_expr;
			g_runtime.last_target_change_ms = now_ms;
		}
	}

	route_expression_toward(state, target, now_ms);
}

/* ── Init ────────────────────────────────────────────────────────── */

void behavior_engine_init(struct pet_state *state, int64_t now_ms)
{
	const int64_t stale_recent_event_ms = now_ms - (60 * MSEC_PER_SEC);
	const int64_t stale_pet_ms = now_ms - (11 * 60 * MSEC_PER_SEC);

	(void)memset(state, 0, sizeof(*state));
	(void)memset(&g_runtime, 0, sizeof(g_runtime));

	/* int8_t -1 is all-bits-one; memset(0) above would leave this as 0 which
	 * is a valid friend slot index.  Force the sentinel explicitly. */
	state->current_active_friend_index = -1;

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

	/* Expression routing starts settled on the boot face. */
	g_runtime.expr_target = PET_EXPR_CALM;
	g_runtime.last_target_change_ms = now_ms;
	g_runtime.last_expr_hop_ms = now_ms;

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
	state->mood = 52;
	state->walk_confidence = 0;
	state->walking_confidence = 0;
	state->notification_burst_level = 0;

	/* Emotional memory: restore the slow relationship traits so this
	 * stays the same pet across power cycles. Values come back lightly
	 * compressed toward the middle — "rested, but it's still me". */
	{
		struct emotion_memory mem;

		emotion_memory_init();
		if (emotion_memory_get(&mem)) {
			state->attachment = (int16_t)CLAMP((int)mem.attachment, 25, 90);
			state->trust = (int16_t)CLAMP((int)mem.trust, 25, 90);
			state->mood = (int16_t)CLAMP((int)mem.mood, 35, 70);
			if (mem.personality < (uint8_t)PET_PERSONALITY_COUNT) {
				g_runtime.personality = (enum pet_personality)mem.personality;
			}
			g_runtime.lifetime_pets = mem.lifetime_pets;
			state->worn_expressive = mem.worn_expressive;
			g_runtime.worn_style_loaded = true;
			LOG_INF("Emotional memory restored: att=%d trust=%d mood=%d pets=%u pers=%s worn=%s",
				state->attachment, state->trust, state->mood,
				g_runtime.lifetime_pets, personality()->name,
				state->worn_expressive ? "expressive" : "quiet");
		}
	}
	fill_memory_record(state, &g_runtime.last_saved_memory);
	g_runtime.last_memory_save_ms = now_ms;

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

	state->carry_context = PET_CARRY_UNKNOWN;
	state->carry_context_confidence = 0U;
	/* Worn style: Kconfig default, overridden by persisted owner choice
	 * in the emotional-memory block above. */
	if (!g_runtime.worn_style_loaded) {
		state->worn_expressive = IS_ENABLED(CONFIG_KERFUR_WORN_EXPRESSIVE);
	}

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

	/* Sleep inertia: leaving deep sleep keeps the pet visibly drowsy
	 * for a while (longer at night). */
	if ((prev_mode == PET_MODE_ASLEEP) && (state->current_mode != PET_MODE_ASLEEP)) {
		const bool night = is_night_time(state, event->timestamp_ms);

		g_runtime.groggy_until_ms = event->timestamp_ms +
			((night ? 90 : 45) * MSEC_PER_SEC);
		LOG_INF("Groggy wake-up (%s)", night ? "night" : "day");
	}

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
	case PET_MODE_WORN:        return "WORN";
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

const char *pet_personality_str(enum pet_personality personality_id)
{
	if (((int)personality_id < 0) ||
	    ((int)personality_id >= (int)PET_PERSONALITY_COUNT)) {
		return "UNKNOWN";
	}

	return g_personalities[personality_id].name;
}

void behavior_engine_emotion_dump(const struct pet_state *state, char *buffer, size_t buffer_len)
{
	if ((buffer == NULL) || (buffer_len == 0U)) {
		return;
	}

	(void)snprintf(buffer, buffer_len,
		       "E=%d Sl=%d At=%d Bo=%d St=%d Ar=%d So=%d Tr=%d Cu=%d "
		       "Mo=%d(acc=%d) ctx=%d/%d/%d intent=%s(%d) groggy=%d "
		       "pers=%s pets=%u carry=%s(%u) sit=%s worn_style=%s",
		       state->energy, state->sleepiness, state->attachment,
		       state->boredom, state->stress, state->arousal,
		       state->social_load, state->trust, state->curiosity,
		       state->mood, g_runtime.mood_accum,
		       g_runtime.ctx.stimulation, g_runtime.ctx.comfort,
		       g_runtime.ctx.social_warmth,
		       pet_intent_str(g_runtime.intent), g_runtime.intent_strength,
		       is_groggy(k_uptime_get()) ? 1 : 0,
		       personality()->name, g_runtime.lifetime_pets,
		       pet_carry_context_str(state->carry_context),
		       state->carry_context_confidence,
		       pet_situation_str(resolve_situation(state)),
		       state->worn_expressive ? "expressive" : "quiet");
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
		"At=%d Ar=%d So=%d Mo=%d ctx=%d/%d/%d intent=%s(%d) pers=%s "
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
		       state->arousal, state->social_load, state->mood,
		       g_runtime.ctx.stimulation, g_runtime.ctx.comfort,
		       g_runtime.ctx.social_warmth,
		       pet_intent_str(g_runtime.intent), g_runtime.intent_strength,
		       personality()->name,
		       (long long)state->last_motion_timestamp_ms,
		       (long long)state->last_walk_timestamp_ms,
		       (long long)state->last_pickup_timestamp_ms,
		       (long long)state->last_in_hand_timestamp_ms,
		       (long long)state->last_still_timestamp_ms,
		       state->dynamic_pupils_forced_disabled ? 1 : 0,
		       state->time_valid ? "valid" : "invalid");
}
