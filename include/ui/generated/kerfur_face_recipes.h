#ifndef KERFUR_FACE_RECIPES_H_
#define KERFUR_FACE_RECIPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/micro_reaction.h"
#include "behavior/pet_state.h"
#include "ui/generated/kerfur_face_assets.h"

#define KERFUR_FACE_MAX_EFFECTS 6

enum kerfur_face_recipe_id {
	KERFUR_FACE_RECIPE_PET_EXPR_CALM = PET_EXPR_CALM,
	KERFUR_FACE_RECIPE_PET_EXPR_CURIOUS = PET_EXPR_CURIOUS,
	KERFUR_FACE_RECIPE_PET_EXPR_CONTENT = PET_EXPR_CONTENT,
	KERFUR_FACE_RECIPE_PET_EXPR_HAPPY = PET_EXPR_HAPPY,
	KERFUR_FACE_RECIPE_PET_EXPR_PLAYFUL = PET_EXPR_PLAYFUL,
	KERFUR_FACE_RECIPE_PET_EXPR_SLEEPY = PET_EXPR_SLEEPY,
	KERFUR_FACE_RECIPE_PET_EXPR_NEEDY = PET_EXPR_NEEDY,
	KERFUR_FACE_RECIPE_PET_EXPR_LONELY = PET_EXPR_LONELY,
	KERFUR_FACE_RECIPE_PET_EXPR_ANNOYED = PET_EXPR_ANNOYED,
	KERFUR_FACE_RECIPE_PET_EXPR_OVERSTIMULATED = PET_EXPR_OVERSTIMULATED,
	KERFUR_FACE_RECIPE_PET_EXPR_COZY = PET_EXPR_COZY,
	KERFUR_FACE_RECIPE_PET_EXPR_DRAINED = PET_EXPR_DRAINED,
	KERFUR_FACE_RECIPE_PET_EXPR_ASLEEP = PET_EXPR_ASLEEP,
	KERFUR_FACE_RECIPE_COUNT = PET_EXPR_ASLEEP + 1
};

enum kerfur_face_reaction_id {
	KERFUR_FACE_REACTION_REACTION_NONE = REACTION_NONE,
	KERFUR_FACE_REACTION_REACTION_BLINK = REACTION_BLINK,
	KERFUR_FACE_REACTION_REACTION_GLANCE_LEFT = REACTION_GLANCE_LEFT,
	KERFUR_FACE_REACTION_REACTION_GLANCE_RIGHT = REACTION_GLANCE_RIGHT,
	KERFUR_FACE_REACTION_REACTION_WAKE_BLINK = REACTION_WAKE_BLINK,
	KERFUR_FACE_REACTION_REACTION_STARTLE = REACTION_STARTLE,
	KERFUR_FACE_REACTION_REACTION_PET_BOW = REACTION_PET_BOW,
	KERFUR_FACE_REACTION_REACTION_HAPPY_BOUNCE = REACTION_HAPPY_BOUNCE,
	KERFUR_FACE_REACTION_REACTION_NOTIF_PING = REACTION_NOTIF_PING,
	KERFUR_FACE_REACTION_REACTION_NOTIF_BURST = REACTION_NOTIF_BURST,
	KERFUR_FACE_REACTION_REACTION_CONNECT_SPARK = REACTION_CONNECT_SPARK,
	KERFUR_FACE_REACTION_REACTION_DISCONNECT_SCAN = REACTION_DISCONNECT_SCAN,
	KERFUR_FACE_REACTION_REACTION_CHARGE_PULSE = REACTION_CHARGE_PULSE,
	KERFUR_FACE_REACTION_REACTION_LOW_BATT_SAG = REACTION_LOW_BATT_SAG,
	KERFUR_FACE_REACTION_REACTION_SLEEP_FADE = REACTION_SLEEP_FADE,
	KERFUR_FACE_REACTION_REACTION_LOOK_UP = REACTION_LOOK_UP,
	KERFUR_FACE_REACTION_REACTION_LOOK_DOWN = REACTION_LOOK_DOWN,
	KERFUR_FACE_REACTION_REACTION_COUNT = REACTION_COUNT,
};

enum kerfur_face_blink_profile_id {
	KERFUR_FACE_BLINK_PROFILE_NONE = 0,
	KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_DEFAULT,
	KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_SLEEPY,
	KERFUR_FACE_BLINK_PROFILE_BLINK_DISABLED,
	KERFUR_FACE_BLINK_PROFILE_COUNT
};

enum kerfur_face_micro_anim_kind {
	KERFUR_FACE_MICRO_ANIM_KIND_NONE = 0,
	KERFUR_FACE_MICRO_ANIM_KIND_LOOK_TARGET,
	KERFUR_FACE_MICRO_ANIM_KIND_PUPIL_MOTION,
	KERFUR_FACE_MICRO_ANIM_KIND_EFFECT_SPAWN,
};

enum kerfur_face_micro_anim_id {
	KERFUR_FACE_MICRO_ANIM_NONE = 0,
	KERFUR_FACE_MICRO_ANIM_PUPIL_IDLE_DRIFT,
	KERFUR_FACE_MICRO_ANIM_GLANCE_LEFT,
	KERFUR_FACE_MICRO_ANIM_GLANCE_RIGHT,
	KERFUR_FACE_MICRO_ANIM_LOOK_UP,
	KERFUR_FACE_MICRO_ANIM_LOOK_DOWN,
	KERFUR_FACE_MICRO_ANIM_TEAR_WELL_UP,
	KERFUR_FACE_MICRO_ANIM_COUNT
};

enum kerfur_face_reaction_compose_mode {
	KERFUR_FACE_REACTION_COMPOSE_OVERLAY = 0,
	KERFUR_FACE_REACTION_COMPOSE_REBASE,
};

enum kerfur_face_override_flags {
	KERFUR_FACE_OVERRIDE_HAS_X = 1 << 0,
	KERFUR_FACE_OVERRIDE_HAS_Y = 1 << 1,
};

struct kerfur_face_point {
	int16_t x;
	int16_t y;
};

struct kerfur_face_override {
	uint8_t flags;
	int16_t x;
	int16_t y;
	int16_t dx;
	int16_t dy;
};

struct kerfur_face_layout {
	struct kerfur_face_point left_eye_white;
	struct kerfur_face_point right_eye_white;
	struct kerfur_face_point left_brow;
	struct kerfur_face_point right_brow;
	struct kerfur_face_point mouth;
	struct kerfur_face_point left_whisker;
	struct kerfur_face_point right_whisker;
	struct kerfur_face_point indicator;
	struct kerfur_face_point overlay;
	struct kerfur_face_point effect;
	struct kerfur_face_point left_eyeball;
	struct kerfur_face_point right_eyeball;
};

struct kerfur_face_effect_instance {
	enum kerfur_face_asset_id asset_id;
	bool has_absolute_position;
	int16_t x;
	int16_t y;
	int16_t dx;
	int16_t dy;
};

struct kerfur_face_recipe {
	const char *name;
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
	enum kerfur_face_indicator_id default_indicator;
	enum kerfur_face_overlay_id default_overlay;
	enum kerfur_face_blink_profile_id blink_profile;
	struct kerfur_face_layout layout;
	int16_t base_look_x;
	int16_t base_look_y;
	uint8_t allow_dynamic_pupils;
	uint8_t special_eye_mode;
	uint8_t dynamic_pupil_scale_x;
	uint8_t dynamic_pupil_scale_y;
	uint8_t dynamic_pupil_speed;
	uint8_t dead_zone_strength;
	uint8_t ambient_motion_count;
	const enum kerfur_face_micro_anim_id *ambient_motion;
	uint8_t default_effect_count;
	const enum kerfur_face_asset_id *default_effects;
};

struct kerfur_face_micro_animation {
	const char *name;
	enum kerfur_face_micro_anim_kind kind;
	uint16_t duration_ms;
	bool loop;
	int16_t target_x;
	int16_t target_y;
	int16_t radius_x;
	int16_t radius_y;
	enum kerfur_face_asset_id effect_asset_id;
	int16_t offset_x;
	int16_t offset_y;
};

struct kerfur_face_reaction {
	const char *name;
	uint16_t duration_ms;
	enum kerfur_face_reaction_compose_mode compose_mode;
	enum pet_expression base_expression;
	int16_t look_offset_x;
	int16_t look_offset_y;
	bool has_allow_dynamic_pupils_override;
	bool allow_dynamic_pupils;
	enum kerfur_face_asset_id left_eye_white;
	enum kerfur_face_asset_id right_eye_white;
	enum kerfur_face_asset_id left_eyeball;
	enum kerfur_face_asset_id right_eyeball;
	enum kerfur_face_asset_id left_brow;
	enum kerfur_face_asset_id right_brow;
	enum kerfur_face_asset_id mouth;
	enum kerfur_face_asset_id whiskers;
	enum kerfur_face_indicator_id indicator;
	enum kerfur_face_overlay_id overlay;
	enum kerfur_face_blink_profile_id blink_override;
	enum kerfur_face_micro_anim_id micro_animation;
	struct kerfur_face_override left_eye_white_override;
	struct kerfur_face_override right_eye_white_override;
	struct kerfur_face_override left_eyeball_override;
	struct kerfur_face_override right_eyeball_override;
	struct kerfur_face_override left_brow_override;
	struct kerfur_face_override right_brow_override;
	struct kerfur_face_override mouth_override;
	struct kerfur_face_override whiskers_override;
	struct kerfur_face_override indicator_override;
	struct kerfur_face_override overlay_override;
	struct kerfur_face_override effect_override;
	uint8_t temporary_effect_count;
	struct kerfur_face_effect_instance temporary_effects[KERFUR_FACE_MAX_EFFECTS];
};

const struct kerfur_face_recipe *kerfur_face_recipe_get(enum kerfur_face_recipe_id id);
const struct kerfur_face_reaction *kerfur_face_reaction_get(enum kerfur_face_reaction_id id);
const struct kerfur_face_micro_animation *kerfur_face_micro_anim_get(enum kerfur_face_micro_anim_id id);
const char *kerfur_face_recipe_name(enum kerfur_face_recipe_id id);
const char *kerfur_face_reaction_name(enum kerfur_face_reaction_id id);

#endif /* KERFUR_FACE_RECIPES_H_ */
