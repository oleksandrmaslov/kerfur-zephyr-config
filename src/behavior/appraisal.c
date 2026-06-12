/*
 * Appraisal — normalized, table-driven expression scoring (see header).
 *
 * Calibration discipline for the weight table:
 *   - every feature is 0..100;
 *   - score = bias + sum(w * f) / 100;
 *   - all rows share one envelope: typical winning scores land 40..95,
 *     extremes stay below ~130. (A row's theoretical positive sum may
 *     exceed that when its features are strongly correlated — e.g.
 *     FRESH_PET implies RECENT_PET — the scenario suite is the real
 *     enforcement, not the column sum.);
 *   - mutually exclusive expressions are separated by their dominant
 *     feature's weight, not by raw scale (the old engine let SLEEPY and
 *     CURIOUS structurally outscore CALM — that class of bug is what
 *     this table fixes).
 *
 * THE NUMBERS BELOW ARE BAKED FROM `tools/appraisal_calibrate.py` — an
 * executable mirror of this table that runs 20 canonical emotional
 * scenarios (boot calm, deep night sleep, fresh petting vs settled
 * contentment, rough handling, notification overload, lonely vs needy,
 * charging cozy, battery low/critical, fresh walk, unknown vs friend
 * Kerfur, worn-quiet, groggy wake, soothed stress, bored poke-me) and
 * asserts the winner and its margin. When changing any weight: edit the
 * script first, make the suite pass, then copy the numbers here.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "behavior/appraisal.h"

#define EXPR_BIT(expr) (1U << (expr))
#define EXPR_ALL_MASK  ((1U << PET_EXPR_COUNT) - 1U)

#define PET_EXPR_COUNT (PET_EXPR_ASLEEP + 1)

BUILD_ASSERT(PET_EXPR_COUNT <= 16, "expression mask is 16 bits");

/* ── Feature extraction ──────────────────────────────────────────── */

static uint8_t clamp_feat(int value)
{
	return (uint8_t)CLAMP(value, 0, 100);
}

/* 100 right after the event, linearly down to 0 at span_ms. */
static uint8_t recency_ramp(int64_t now_ms, int64_t event_ms, int64_t span_ms)
{
	int64_t age;

	if (event_ms <= 0LL || span_ms <= 0LL) {
		return 0U;
	}
	age = now_ms - event_ms;
	if (age <= 0LL) {
		return 100U;
	}
	if (age >= span_ms) {
		return 0U;
	}
	return (uint8_t)(100LL - (age * 100LL) / span_ms);
}

void appraisal_compute_features(const struct appraisal_inputs *in,
				uint8_t features[APF_COUNT])
{
	const struct pet_state *s = in->state;
	const int64_t now = in->now_ms;
	int64_t neglect_ms;
	bool long_disconnect;

	features[APF_ENERGY] = clamp_feat(s->energy);
	features[APF_SLEEPINESS] = clamp_feat(s->sleepiness);
	features[APF_ATTACHMENT] = clamp_feat(s->attachment);
	features[APF_BOREDOM] = clamp_feat(s->boredom);
	features[APF_STRESS] = clamp_feat(s->stress);
	features[APF_AROUSAL] = clamp_feat(s->arousal);
	features[APF_SOCIAL_LOAD] = clamp_feat(s->social_load);
	features[APF_TRUST] = clamp_feat(s->trust);
	features[APF_CURIOSITY] = clamp_feat(s->curiosity);

	features[APF_MOOD_HIGH] = clamp_feat((s->mood - 50) * 2);
	features[APF_MOOD_LOW] = clamp_feat((50 - s->mood) * 2);

	features[APF_COMFORT] = clamp_feat(in->ctx_comfort);
	features[APF_STIMULATION] = clamp_feat(in->ctx_stimulation);
	features[APF_SOCIAL_WARMTH] = clamp_feat(in->ctx_social_warmth);

	features[APF_FRESH_PET] =
		recency_ramp(now, s->last_pet_timestamp_ms, 8LL * MSEC_PER_SEC);
	features[APF_RECENT_PET] =
		recency_ramp(now, s->last_pet_timestamp_ms, 10LL * 60 * MSEC_PER_SEC);
	features[APF_RECENT_MOTION] =
		recency_ramp(now, s->last_motion_timestamp_ms, 45LL * MSEC_PER_SEC);
	features[APF_RECENT_NOTIF] =
		recency_ramp(now, s->last_phone_event_timestamp_ms, 30LL * MSEC_PER_SEC);
	features[APF_ROUGH_RECENT] =
		recency_ramp(now, s->last_rough_event_timestamp_ms, 60LL * MSEC_PER_SEC);

	/* Neglect: 0 until 2 min without real interaction, 100 at 30 min. */
	neglect_ms = now - s->last_real_interaction_timestamp_ms;
	if (neglect_ms <= (2LL * 60 * MSEC_PER_SEC)) {
		features[APF_NEGLECT] = 0U;
	} else {
		features[APF_NEGLECT] = clamp_feat(
			(int)((neglect_ms - (2LL * 60 * MSEC_PER_SEC)) /
			      ((28LL * 60 * MSEC_PER_SEC) / 100LL)));
	}

	long_disconnect = !s->ble_connected &&
			  ((now - s->last_phone_event_timestamp_ms) >
			   (30LL * 60 * MSEC_PER_SEC));
	features[APF_LONG_DISCONNECT] = long_disconnect ? 100U : 0U;
	features[APF_NIGHT] = in->night ? 100U : 0U;
	features[APF_GROGGY] = in->groggy ? 100U : 0U;
	features[APF_BATT_LOW] = s->battery_low ? 100U : 0U;
	features[APF_BATT_CRITICAL] = s->battery_critical ? 100U : 0U;
	features[APF_CHARGING] = s->charging ? 100U : 0U;

	/* Walk novelty: a fresh walk is exciting, a long one is routine. */
	if ((s->current_mode == PET_MODE_WALK_AWAKE) &&
	    (s->walking_session_start_ms > 0LL)) {
		int64_t walk_s = (now - s->walking_session_start_ms) / MSEC_PER_SEC;

		if (walk_s < 120) {
			features[APF_WALK_NOVELTY] = 100U;
		} else if (walk_s < 600) {
			features[APF_WALK_NOVELTY] =
				clamp_feat(100 - (int)((walk_s - 120) * 80 / 480));
		} else {
			features[APF_WALK_NOVELTY] = 20U;
		}
	} else {
		features[APF_WALK_NOVELTY] = 0U;
	}

	features[APF_BURST] =
		clamp_feat((int)s->notification_burst_level * 12);
}

/* ── Weight table ────────────────────────────────────────────────── */

struct appraisal_row {
	int8_t bias;
	int8_t w[APF_COUNT];
};

/* Designated feature indices keep rows readable. */
#define W(feature, weight) [feature] = (weight)

static const struct appraisal_row g_appraisal[PET_EXPR_COUNT] = {
	/* CALM — the default resting face. Wins when nothing pulls away. */
	[PET_EXPR_CALM] = {
		.bias = 52,
		.w = {
			W(APF_STRESS, -85), W(APF_SOCIAL_LOAD, -40),
			W(APF_BOREDOM, -22), W(APF_ENERGY, 22),
			W(APF_MOOD_HIGH, 10), W(APF_MOOD_LOW, -8),
			W(APF_STIMULATION, -12), W(APF_NIGHT, 4),
		},
	},
	/* CURIOUS — interest in the world; fed by novelty, killed by sleep. */
	[PET_EXPR_CURIOUS] = {
		.bias = 6,
		.w = {
			W(APF_CURIOSITY, 62), W(APF_RECENT_NOTIF, 14),
			W(APF_RECENT_MOTION, 11), W(APF_WALK_NOVELTY, 16),
			W(APF_STIMULATION, 10), W(APF_SLEEPINESS, -30),
		},
	},
	/* CONTENT — secure bond at rest. */
	[PET_EXPR_CONTENT] = {
		.bias = 6,
		.w = {
			W(APF_ATTACHMENT, 40), W(APF_TRUST, 38),
			W(APF_RECENT_PET, 16), W(APF_STRESS, -45),
			W(APF_MOOD_HIGH, 12), W(APF_MOOD_LOW, -8),
			W(APF_COMFORT, 18),
		},
	},
	/* HAPPY — active joy: spikes after petting, and social warmth +
	 * arousal let a friend's arrival read as joy, not just play. */
	[PET_EXPR_HAPPY] = {
		.bias = 0,
		.w = {
			W(APF_ATTACHMENT, 42), W(APF_ENERGY, 24),
			W(APF_RECENT_PET, 22), W(APF_FRESH_PET, 26),
			W(APF_STRESS, -45), W(APF_MOOD_HIGH, 16),
			W(APF_MOOD_LOW, -12), W(APF_COMFORT, 18),
			W(APF_SOCIAL_WARMTH, 14), W(APF_AROUSAL, 10),
		},
	},
	/* PLAYFUL — high arousal with trust behind it. */
	[PET_EXPR_PLAYFUL] = {
		.bias = 0,
		.w = {
			W(APF_AROUSAL, 75), W(APF_TRUST, 28),
			W(APF_WALK_NOVELTY, 12), W(APF_MOOD_HIGH, 10),
			W(APF_MOOD_LOW, -8), W(APF_BATT_LOW, -20),
			W(APF_STIMULATION, 10), W(APF_SLEEPINESS, -15),
		},
	},
	/* SLEEPY — calibrated so it competes with, not crushes, CALM. */
	[PET_EXPR_SLEEPY] = {
		.bias = 0,
		.w = {
			W(APF_SLEEPINESS, 78), W(APF_AROUSAL, -18),
			W(APF_NIGHT, 8), W(APF_NEGLECT, 10),
			W(APF_GROGGY, 14), W(APF_CHARGING, 6),
		},
	},
	/* NEEDY — wants attention now; warmth afterglow soothes it. */
	[PET_EXPR_NEEDY] = {
		.bias = 6,
		.w = {
			W(APF_BOREDOM, 55), W(APF_ATTACHMENT, 18),
			W(APF_RECENT_PET, -20), W(APF_NEGLECT, 18),
			W(APF_MOOD_LOW, 10), W(APF_MOOD_HIGH, -6),
			W(APF_SOCIAL_WARMTH, -22), W(APF_COMFORT, -16),
		},
	},
	/* LONELY — long emptiness, no phone, nothing moving. */
	[PET_EXPR_LONELY] = {
		.bias = 0,
		.w = {
			W(APF_BOREDOM, 48), W(APF_LONG_DISCONNECT, 20),
			W(APF_RECENT_MOTION, -10), W(APF_NEGLECT, 26),
			W(APF_MOOD_LOW, 14), W(APF_MOOD_HIGH, -10),
			W(APF_SOCIAL_WARMTH, -22), W(APF_COMFORT, -14),
		},
	},
	/* ANNOYED — stress with a rough-handling edge; trust dampens. */
	[PET_EXPR_ANNOYED] = {
		.bias = 8,
		.w = {
			W(APF_STRESS, 70), W(APF_ROUGH_RECENT, 24),
			W(APF_TRUST, -16), W(APF_MOOD_LOW, 8),
			W(APF_MOOD_HIGH, -6), W(APF_STIMULATION, 14),
		},
	},
	/* OVERSTIMULATED — too much world at once. */
	[PET_EXPR_OVERSTIMULATED] = {
		.bias = 0,
		.w = {
			W(APF_SOCIAL_LOAD, 60), W(APF_BURST, 35),
			W(APF_AROUSAL, 25), W(APF_STIMULATION, 18),
		},
	},
	/* COZY — charging warmth / settled comfort. */
	[PET_EXPR_COZY] = {
		.bias = 8,
		.w = {
			W(APF_CHARGING, 52), W(APF_STRESS, -30),
			W(APF_RECENT_PET, 10), W(APF_COMFORT, 24),
			W(APF_SLEEPINESS, 8), W(APF_NIGHT, 4),
		},
	},
	/* DRAINED — battery exhaustion dominates everything else. */
	[PET_EXPR_DRAINED] = {
		.bias = 24,
		.w = {
			W(APF_BATT_CRITICAL, 62), W(APF_BATT_LOW, 24),
			W(APF_ENERGY, -24), W(APF_SLEEPINESS, 28),
		},
	},
	/* ASLEEP — selected by the mode override in the engine, never by
	 * scoring. Negative bias keeps it from winning a degenerate round
	 * where every other row went negative. */
	[PET_EXPR_ASLEEP] = {
		.bias = -100,
		.w = { 0 },
	},
};

/* ── Situation policies ──────────────────────────────────────────── */

struct situation_policy {
	const char *name;
	uint16_t allowed_mask;
	int8_t bias[PET_EXPR_COUNT];
};

static const struct situation_policy g_situations[PET_SITUATION_COUNT] = {
	[PET_SITUATION_RESTING] = {
		.name = "RESTING",
		.allowed_mask = EXPR_ALL_MASK,
		.bias = {
			[PET_EXPR_CALM] = 6,
			[PET_EXPR_SLEEPY] = 4,
		},
	},
	/* In the hand: being held is attention — loneliness is impossible
	 * and the pet leans bright. */
	[PET_SITUATION_ENGAGED] = {
		.name = "ENGAGED",
		.allowed_mask = EXPR_ALL_MASK,
		.bias = {
			[PET_EXPR_CURIOUS] = 8,
			[PET_EXPR_HAPPY] = 6,
			[PET_EXPR_PLAYFUL] = 6,
			[PET_EXPR_SLEEPY] = -10,
			[PET_EXPR_LONELY] = -20,
			[PET_EXPR_NEEDY] = -10,
		},
	},
	/* Worn quietly: the display only wakes for moments that matter
	 * (grabs, peers, charger), so keep the face inventory simple. */
	[PET_SITUATION_WORN_QUIET] = {
		.name = "WORN_QUIET",
		.allowed_mask = EXPR_BIT(PET_EXPR_CALM) |
				EXPR_BIT(PET_EXPR_CONTENT) |
				EXPR_BIT(PET_EXPR_CURIOUS) |
				EXPR_BIT(PET_EXPR_SLEEPY) |
				EXPR_BIT(PET_EXPR_DRAINED) |
				EXPR_BIT(PET_EXPR_ASLEEP),
		.bias = {
			[PET_EXPR_CALM] = 8,
			[PET_EXPR_CONTENT] = 4,
		},
	},
	/* Worn but expressive: along for the adventure. */
	[PET_SITUATION_WORN_LIVELY] = {
		.name = "WORN_LIVELY",
		.allowed_mask = EXPR_ALL_MASK,
		.bias = {
			[PET_EXPR_CURIOUS] = 6,
			[PET_EXPR_PLAYFUL] = 4,
			[PET_EXPR_LONELY] = -15,
			[PET_EXPR_NEEDY] = -10,
		},
	},
	[PET_SITUATION_CHARGING] = {
		.name = "CHARGING",
		.allowed_mask = EXPR_ALL_MASK,
		.bias = {
			[PET_EXPR_COZY] = 10,
			[PET_EXPR_CALM] = 6,
			[PET_EXPR_SLEEPY] = 4,
			[PET_EXPR_PLAYFUL] = -10,
		},
	},
	/* Another Kerfur is here — be present for it. Joy outranks raw
	 * play so a friend's arrival greets, not just bounces. */
	[PET_SITUATION_SOCIAL] = {
		.name = "SOCIAL",
		.allowed_mask = EXPR_ALL_MASK,
		.bias = {
			[PET_EXPR_CURIOUS] = 10,
			[PET_EXPR_HAPPY] = 8,
			[PET_EXPR_PLAYFUL] = 3,
			[PET_EXPR_SLEEPY] = -10,
			[PET_EXPR_LONELY] = -25,
			[PET_EXPR_NEEDY] = -15,
		},
	},
};

/* ── Scoring ─────────────────────────────────────────────────────── */

static enum pet_situation sanitize_situation(enum pet_situation situation)
{
	if ((int)situation < 0 || (int)situation >= (int)PET_SITUATION_COUNT) {
		return PET_SITUATION_RESTING;
	}
	return situation;
}

int appraisal_expression_score(enum pet_expression expr,
			       enum pet_situation situation,
			       const uint8_t features[APF_COUNT])
{
	const struct appraisal_row *row;
	const struct situation_policy *policy;
	int32_t acc = 0;
	int f;

	if ((int)expr < 0 || (int)expr >= PET_EXPR_COUNT) {
		return 0;
	}

	row = &g_appraisal[expr];
	policy = &g_situations[sanitize_situation(situation)];

	for (f = 0; f < APF_COUNT; f++) {
		acc += (int32_t)row->w[f] * (int32_t)features[f];
	}

	return (int)(row->bias + policy->bias[expr] + (acc / 100));
}

bool appraisal_expression_allowed(enum pet_expression expr,
				  enum pet_situation situation)
{
	const struct situation_policy *policy =
		&g_situations[sanitize_situation(situation)];

	if ((int)expr < 0 || (int)expr >= PET_EXPR_COUNT) {
		return false;
	}
	/* CALM is the guaranteed fallback everywhere. */
	if (expr == PET_EXPR_CALM) {
		return true;
	}
	return (policy->allowed_mask & EXPR_BIT(expr)) != 0U;
}

const char *pet_situation_str(enum pet_situation situation)
{
	return g_situations[sanitize_situation(situation)].name;
}

const char *pet_carry_context_str(enum pet_carry_context context)
{
	switch (context) {
	case PET_CARRY_ON_SURFACE: return "SURFACE";
	case PET_CARRY_IN_HAND:    return "IN_HAND";
	case PET_CARRY_WORN:       return "WORN";
	case PET_CARRY_TRANSITION: return "TRANSIT";
	case PET_CARRY_UNKNOWN:
	default:                   return "UNKNOWN";
	}
}
