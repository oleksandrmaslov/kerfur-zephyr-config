#ifndef KERFUR_FACE_RUNTIME_H_
#define KERFUR_FACE_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "behavior/pet_state.h"
#include "ui/generated/kerfur_face_recipes.h"

#define KERFUR_FACE_MAX_INDICATORS 3

enum face_pupil_swap_style {
	FACE_PUPIL_SWAP_INSTANT = 0,
	FACE_PUPIL_SWAP_ON_BLINK,
	FACE_PUPIL_SWAP_SETTLE,
};

enum face_runtime_dynamic_reason {
	FACE_RUNTIME_DYNAMIC_ALLOWED = 0,
	FACE_RUNTIME_DYNAMIC_DISABLED_RECIPE,
	FACE_RUNTIME_DYNAMIC_DISABLED_REACTION,
	FACE_RUNTIME_DYNAMIC_DISABLED_BLINK,
	FACE_RUNTIME_DYNAMIC_DISABLED_NO_ZONE,
	FACE_RUNTIME_DYNAMIC_DISABLED_SPECIAL,
	FACE_RUNTIME_DYNAMIC_DISABLED_NO_PUPIL,
	FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_WALKING,
	FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_ROUGH,
	FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_LOW_CONFIDENCE,
	FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND,
	FACE_RUNTIME_DYNAMIC_DISABLED_BATTERY_SAVE,
	FACE_RUNTIME_DYNAMIC_DISABLED_FORCED_OFF,
};

struct face_runtime_effect_draw {
	enum kerfur_face_asset_id asset_id;
	struct kerfur_face_point position;
};

struct face_runtime_plan {
	enum kerfur_face_recipe_id recipe_id;
	enum kerfur_face_reaction_id reaction_id;
	enum kerfur_face_indicator_id indicator_id;
	enum kerfur_face_overlay_id overlay_id;
	enum kerfur_face_blink_profile_id blink_profile_id;
	enum kerfur_face_asset_id left_eye_white;
	enum kerfur_face_asset_id right_eye_white;
	enum kerfur_face_asset_id blink_left_eye_white;
	enum kerfur_face_asset_id blink_right_eye_white;
	enum kerfur_face_asset_id left_eyeball;
	enum kerfur_face_asset_id right_eyeball;
	enum kerfur_face_asset_id left_brow;
	enum kerfur_face_asset_id right_brow;
	enum kerfur_face_asset_id mouth;
	enum kerfur_face_asset_id whiskers;
	enum kerfur_face_asset_id indicator_asset;
	enum kerfur_face_indicator_id indicator_ids[KERFUR_FACE_MAX_INDICATORS];
	uint8_t indicator_count;
	enum kerfur_face_asset_id overlay_asset;
	struct kerfur_face_layout layout;
	struct face_runtime_effect_draw effects[KERFUR_FACE_MAX_EFFECTS];
	uint8_t effect_count;
	int16_t look_target_x;
	int16_t look_target_y;
	int16_t look_render_x;
	int16_t look_render_y;
	int16_t left_pupil_offset_x;
	int16_t left_pupil_offset_y;
	int16_t right_pupil_offset_x;
	int16_t right_pupil_offset_y;
	bool dynamic_pupils_allowed;
	bool special_eye_mode_active;
	bool blink_left_active;
	bool blink_right_active;
	enum face_runtime_dynamic_reason dynamic_reason;
	int16_t left_brow_dy;
	int16_t right_brow_dy;
	int16_t mouth_dy;
	int16_t left_whisker_dy;
	int16_t right_whisker_dy;
	/* Transition easing offsets for the eye-white (and the pupils that
	 * ride on it): non-zero only mid-transition, eased to 0 so the eyes
	 * glide into a new expression instead of teleporting. */
	int16_t left_eye_dx;
	int16_t left_eye_dy;
	int16_t right_eye_dx;
	int16_t right_eye_dy;
	uint8_t eye_openness;
	uint8_t left_eye_openness;
	uint8_t right_eye_openness;
	bool per_eye_openness;
	int16_t tear_drift_dy;
	bool transition_active;
};

struct face_runtime_state {
	enum kerfur_face_recipe_id current_expression_id;
	enum kerfur_face_reaction_id current_reaction_id;
	enum kerfur_face_indicator_id current_indicator_id;
	enum kerfur_face_overlay_id current_overlay_id;
	int16_t look_target_x;
	int16_t look_target_y;
	int16_t look_render_x;
	int16_t look_render_y;
	uint8_t look_confidence;
	bool dynamic_pupils_allowed;
	bool special_eye_mode_active;
	bool blink_active;
	bool in_hand;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t walking_confidence;
	int8_t battery_percent;
	bool battery_percent_known;
	int64_t last_step_ms;
	struct face_runtime_plan plan;

	/* Transition detection */
	enum kerfur_face_recipe_id prev_expression_id;
	int64_t transition_start_ms;
	bool transition_active;

	/* Interpolation channels: current smoothed values */
	int16_t ch_left_brow_dy;
	int16_t ch_right_brow_dy;
	int16_t ch_mouth_dy;
	int16_t ch_left_whisker_dy;
	int16_t ch_right_whisker_dy;
	int16_t ch_eye_openness; /* 0..100 */
	/* Eye-white position easing channels (target is always 0). */
	int16_t ch_left_eye_dx;
	int16_t ch_left_eye_dy;
	int16_t ch_right_eye_dx;
	int16_t ch_right_eye_dy;

	/* Channel targets */
	int16_t tgt_left_brow_dy;
	int16_t tgt_right_brow_dy;
	int16_t tgt_mouth_dy;
	int16_t tgt_left_whisker_dy;
	int16_t tgt_right_whisker_dy;
	int16_t tgt_eye_openness;

	/* Whisker wiggle animation state */
	int64_t whisker_anim_start_ms;

	/* Blink rework state */
	int64_t blink_next_ms;
	int64_t blink_phase_start_ms;
	bool blink_descending;

	/* Pupil swap state */
	enum face_pupil_swap_style pending_pupil_swap;
	bool pupil_swap_waiting_for_blink;
	enum kerfur_face_asset_id deferred_left_eyeball;
	enum kerfur_face_asset_id deferred_right_eyeball;
	int64_t pupil_settle_start_ms;

	/* NOTIF_PING wink state */
	bool notif_wink_left_closed;
	int64_t notif_wink_last_toggle_ms;

	/* ANNOYED jitter state */
	int16_t jitter_offset_x;
	int16_t jitter_offset_y;
	uint16_t jitter_seed;

	/* Tear drift state */
	int16_t tear_drift_dy;
	int64_t tear_last_spawn_ms;

	/* Reaction stagger state */
	int64_t reaction_enter_ms;
	uint8_t reaction_stagger_phase;

	/* Context bias (Stage 4) */
	int16_t ctx_openness_bias;
	int16_t ctx_brow_bias_dy;
	int16_t ctx_mouth_bias_dy;

	/* Organic micro-life (Stage 5) */
	bool blink_double_pending;
	bool wander_active;
	int16_t wander_x;
	int16_t wander_y;
	int64_t wander_next_ms;
	int64_t wander_hold_until_ms;
};

void face_runtime_init(struct face_runtime_state *runtime);
const struct face_runtime_plan *face_runtime_step(struct face_runtime_state *runtime,
						  struct pet_state *state,
						  int64_t now_ms,
						  bool ambient,
						  bool debug_dump_requested);
const char *face_runtime_dynamic_reason_str(enum face_runtime_dynamic_reason reason);

#if IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG)
void face_runtime_debug_force_blink(struct face_runtime_state *runtime);
void face_runtime_debug_force_openness(struct face_runtime_state *runtime, uint8_t openness);
#endif

#endif /* KERFUR_FACE_RUNTIME_H_ */
