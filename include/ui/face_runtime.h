#ifndef KERFUR_FACE_RUNTIME_H_
#define KERFUR_FACE_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/pet_state.h"
#include "ui/generated/kerfur_face_recipes.h"

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
};

void face_runtime_init(struct face_runtime_state *runtime);
const struct face_runtime_plan *face_runtime_step(struct face_runtime_state *runtime,
						  struct pet_state *state,
						  int64_t now_ms,
						  bool ambient,
						  bool debug_dump_requested);
const char *face_runtime_dynamic_reason_str(enum face_runtime_dynamic_reason reason);

#endif /* KERFUR_FACE_RUNTIME_H_ */
