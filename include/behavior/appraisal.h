#ifndef KERFUR_APPRAISAL_H_
#define KERFUR_APPRAISAL_H_

/*
 * Appraisal — the normalized, table-driven expression scoring layer.
 *
 * Replaces the old hand-written per-expression formulas, which mixed
 * incommensurate scales (some expressions could structurally never win,
 * others could exceed 130) and spread ~60 magic numbers across a switch.
 *
 * Model:
 *   1. The engine builds a feature vector. Every feature is normalized
 *      to 0..100 (drives pass through, recencies become linear ramps,
 *      booleans become 0/100).
 *   2. score(expr) = bias[expr] + sum(weight[expr][f] * feature[f]) / 100
 *      with all weight rows calibrated to the same envelope (typical
 *      winning scores 40..90, hard ceiling ~130).
 *   3. The deterministic situation (enum pet_situation) adds a small
 *      per-situation bias and an allowed-expression mask, so the same
 *      emotional state reads differently on a desk, in a hand, or on a
 *      backpack strap.
 *
 * The engine keeps intent alignment, transition affinity and hysteresis
 * on top — appraisal is a pure, unit-testable scoring function.
 */

#include <stdbool.h>
#include <stdint.h>

#include "behavior/pet_state.h"

enum appraisal_feature {
	/* Drives (pass-through 0..100) */
	APF_ENERGY = 0,
	APF_SLEEPINESS,
	APF_ATTACHMENT,
	APF_BOREDOM,
	APF_STRESS,
	APF_AROUSAL,
	APF_SOCIAL_LOAD,
	APF_TRUST,
	APF_CURIOSITY,
	/* Mood split into two one-sided 0..100 features */
	APF_MOOD_HIGH,
	APF_MOOD_LOW,
	/* Afterglow context (0..100) */
	APF_COMFORT,
	APF_STIMULATION,
	APF_SOCIAL_WARMTH,
	/* Recency ramps (100 right after the event, linear to 0) */
	APF_FRESH_PET,        /* 8 s */
	APF_RECENT_PET,       /* 10 min */
	APF_RECENT_MOTION,    /* 45 s */
	APF_RECENT_NOTIF,     /* 30 s */
	APF_ROUGH_RECENT,     /* 60 s */
	APF_NEGLECT,          /* 0 at <=2 min without interaction, 100 at 30 min */
	/* Booleans (0/100) */
	APF_LONG_DISCONNECT,
	APF_NIGHT,
	APF_GROGGY,
	APF_BATT_LOW,
	APF_BATT_CRITICAL,
	APF_CHARGING,
	/* Activity */
	APF_WALK_NOVELTY,     /* early-walk freshness, decays over the session */
	APF_BURST,            /* notification burst pressure */
	APF_COUNT
};

struct appraisal_inputs {
	const struct pet_state *state;
	/* Afterglow context owned by the behavior engine runtime. */
	int16_t ctx_stimulation;
	int16_t ctx_comfort;
	int16_t ctx_social_warmth;
	bool groggy;
	bool night;
	int64_t now_ms;
};

/* Build the normalized feature vector. */
void appraisal_compute_features(const struct appraisal_inputs *in,
				uint8_t features[APF_COUNT]);

/* Score one expression for the given situation. Returns a value on the
 * shared 0..~130 envelope (may be slightly negative for strongly
 * counter-indicated expressions). */
int appraisal_expression_score(enum pet_expression expr,
			       enum pet_situation situation,
			       const uint8_t features[APF_COUNT]);

/* Whether the situation allows this expression at all. CALM is always
 * allowed in every situation (guaranteed fallback). */
bool appraisal_expression_allowed(enum pet_expression expr,
				  enum pet_situation situation);

const char *pet_situation_str(enum pet_situation situation);
const char *pet_carry_context_str(enum pet_carry_context context);

#endif /* KERFUR_APPRAISAL_H_ */
