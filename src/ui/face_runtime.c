#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ui/face_runtime.h"
#include "ui/generated/kerfur_face_assets.h"

LOG_MODULE_REGISTER(face_runtime, CONFIG_LOG_DEFAULT_LEVEL);

#define FACE_BLINK_PERIOD_FOREGROUND_MS 4200LL
#define FACE_BLINK_PERIOD_AMBIENT_MS 8000LL
#define FACE_BLINK_DURATION_DEFAULT_MS 220LL
#define FACE_BLINK_DURATION_SLEEPY_MS 400LL

static int16_t clamp_s16(int value, int min_value, int max_value)
{
	if (value < min_value) {
		return (int16_t)min_value;
	}
	if (value > max_value) {
		return (int16_t)max_value;
	}
	return (int16_t)value;
}

static int sign_i16(int16_t value)
{
	if (value < 0) {
		return -1;
	}
	if (value > 0) {
		return 1;
	}
	return 0;
}

static int abs_i16(int16_t value)
{
	return (value < 0) ? -value : value;
}

static int16_t percent_to_offset(int16_t percent, int16_t max_offset)
{
	int value;

	if ((percent == 0) || (max_offset <= 0)) {
		return 0;
	}

	value = percent * max_offset;
	if (value >= 0) {
		value = (value + 50) / 100;
	} else {
		value = (value - 50) / 100;
	}

	if ((value == 0) && (abs_i16(percent) >= 18)) {
		value = sign_i16(percent);
	}

	return clamp_s16(value, -max_offset, max_offset);
}

static uint8_t motion_scale_for_recipe(enum kerfur_face_recipe_id recipe_id)
{
	switch (recipe_id) {
	case KERFUR_FACE_RECIPE_PET_EXPR_PLAYFUL:
		return 75U;
	case KERFUR_FACE_RECIPE_PET_EXPR_COZY:
		return 60U;
	case KERFUR_FACE_RECIPE_PET_EXPR_SLEEPY:
		return 35U;
	case KERFUR_FACE_RECIPE_PET_EXPR_NEEDY:
	case KERFUR_FACE_RECIPE_PET_EXPR_LONELY:
		return 85U;
	case KERFUR_FACE_RECIPE_PET_EXPR_OVERSTIMULATED:
	case KERFUR_FACE_RECIPE_PET_EXPR_ASLEEP:
		return 0U;
	default:
		return 100U;
	}
}

static uint8_t motion_speed_for_recipe(enum kerfur_face_recipe_id recipe_id)
{
	switch (recipe_id) {
	case KERFUR_FACE_RECIPE_PET_EXPR_PLAYFUL:
		return 65U;
	case KERFUR_FACE_RECIPE_PET_EXPR_COZY:
		return 35U;
	case KERFUR_FACE_RECIPE_PET_EXPR_SLEEPY:
		return 18U;
	case KERFUR_FACE_RECIPE_PET_EXPR_ASLEEP:
		return 0U;
	default:
		return 55U;
	}
}

static struct kerfur_face_point apply_override_point(struct kerfur_face_point point,
						     const struct kerfur_face_override *override)
{
	if (override == NULL) {
		return point;
	}

	if ((override->flags & KERFUR_FACE_OVERRIDE_HAS_X) != 0U) {
		point.x = override->x;
	}
	if ((override->flags & KERFUR_FACE_OVERRIDE_HAS_Y) != 0U) {
		point.y = override->y;
	}

	point.x += override->dx;
	point.y += override->dy;
	return point;
}

static enum kerfur_face_recipe_id sanitize_recipe_id(enum pet_expression expression)
{
	if ((expression < PET_EXPR_CALM) || (expression >= PET_EXPR_ASLEEP + 1)) {
		return KERFUR_FACE_RECIPE_PET_EXPR_CALM;
	}

	return (enum kerfur_face_recipe_id)expression;
}

static enum kerfur_face_reaction_id sanitize_reaction_id(enum micro_reaction_type reaction)
{
	if ((reaction < REACTION_NONE) || (reaction >= REACTION_COUNT)) {
		return KERFUR_FACE_REACTION_REACTION_NONE;
	}

	return (enum kerfur_face_reaction_id)reaction;
}

static enum kerfur_face_indicator_id sanitize_indicator_id(int16_t raw, bool *has_override)
{
	if (has_override != NULL) {
		*has_override = false;
	}

	if (raw < 0) {
		return KERFUR_FACE_INDICATOR_NONE;
	}
	if (raw >= KERFUR_FACE_INDICATOR_COUNT) {
		return KERFUR_FACE_INDICATOR_NONE;
	}

	if (has_override != NULL) {
		*has_override = true;
	}
	return (enum kerfur_face_indicator_id)raw;
}

static enum kerfur_face_overlay_id sanitize_overlay_id(int16_t raw, bool *has_override)
{
	if (has_override != NULL) {
		*has_override = false;
	}

	if (raw < 0) {
		return KERFUR_FACE_OVERLAY_NONE;
	}
	if (raw >= KERFUR_FACE_OVERLAY_COUNT) {
		return KERFUR_FACE_OVERLAY_NONE;
	}

	if (has_override != NULL) {
		*has_override = true;
	}
	return (enum kerfur_face_overlay_id)raw;
}

static enum kerfur_face_asset_id choose_asset(enum kerfur_face_asset_id base_asset,
					      enum kerfur_face_asset_id override_asset)
{
	return (override_asset != KERFUR_FACE_ASSET_NONE) ? override_asset : base_asset;
}

static enum kerfur_face_indicator_id resolve_indicator(const struct pet_state *state,
						       const struct kerfur_face_recipe *recipe,
						       const struct kerfur_face_reaction *reaction)
{
	bool explicit_override;
	enum kerfur_face_indicator_id indicator =
		sanitize_indicator_id(state->current_indicator, &explicit_override);

	if (!explicit_override) {
		indicator = recipe->default_indicator;
		if ((indicator == KERFUR_FACE_INDICATOR_NONE) && state->ble_connected) {
			indicator = KERFUR_FACE_INDICATOR_ICON_BT;
		}
	}

	if (reaction->indicator != KERFUR_FACE_INDICATOR_NONE) {
		indicator = reaction->indicator;
	}

	return indicator;
}

static enum kerfur_face_overlay_id resolve_overlay(const struct pet_state *state,
						   const struct kerfur_face_recipe *recipe,
						   const struct kerfur_face_reaction *reaction)
{
	bool explicit_override;
	enum kerfur_face_overlay_id overlay =
		sanitize_overlay_id(state->current_overlay, &explicit_override);

	if (!explicit_override) {
		overlay = recipe->default_overlay;
		if ((overlay == KERFUR_FACE_OVERLAY_NONE) && state->charging) {
			overlay = KERFUR_FACE_OVERLAY_OVERLAY_BATTERY_PERCENT;
		}
	}

	if (reaction->overlay != KERFUR_FACE_OVERLAY_NONE) {
		overlay = reaction->overlay;
	}

	return overlay;
}

static struct kerfur_face_layout resolve_layout(const struct kerfur_face_recipe *recipe,
						const struct kerfur_face_reaction *reaction)
{
	struct kerfur_face_layout layout = recipe->layout;

	layout.left_eye_white =
		apply_override_point(layout.left_eye_white, &reaction->left_eye_white_override);
	layout.right_eye_white =
		apply_override_point(layout.right_eye_white, &reaction->right_eye_white_override);
	layout.left_eyeball =
		apply_override_point(layout.left_eyeball, &reaction->left_eyeball_override);
	layout.right_eyeball =
		apply_override_point(layout.right_eyeball, &reaction->right_eyeball_override);
	layout.left_brow = apply_override_point(layout.left_brow, &reaction->left_brow_override);
	layout.right_brow = apply_override_point(layout.right_brow, &reaction->right_brow_override);
	layout.mouth = apply_override_point(layout.mouth, &reaction->mouth_override);
	layout.left_whisker =
		apply_override_point(layout.left_whisker, &reaction->whiskers_override);
	layout.right_whisker =
		apply_override_point(layout.right_whisker, &reaction->whiskers_override);
	layout.indicator = apply_override_point(layout.indicator, &reaction->indicator_override);
	layout.overlay = apply_override_point(layout.overlay, &reaction->overlay_override);
	layout.effect = apply_override_point(layout.effect, &reaction->effect_override);

	return layout;
}

static enum kerfur_face_blink_profile_id resolve_blink_profile(
	const struct kerfur_face_recipe *recipe,
	const struct kerfur_face_reaction *reaction)
{
	if (reaction->blink_override != KERFUR_FACE_BLINK_PROFILE_NONE) {
		return reaction->blink_override;
	}

	return recipe->blink_profile;
}

static bool reaction_has_visual_activity(enum kerfur_face_reaction_id reaction_id)
{
	return reaction_id != KERFUR_FACE_REACTION_REACTION_NONE;
}

static bool blink_profile_is_sleepy(enum kerfur_face_blink_profile_id profile)
{
	return profile == KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_SLEEPY;
}

static bool blink_profile_is_disabled(enum kerfur_face_blink_profile_id profile)
{
	return profile == KERFUR_FACE_BLINK_PROFILE_BLINK_DISABLED;
}

static bool should_blink(const struct pet_state *state,
			 enum kerfur_face_reaction_id reaction_id,
			 enum kerfur_face_blink_profile_id profile,
			 int64_t now_ms,
			 bool ambient)
{
	const int64_t blink_period_ms =
		ambient ? FACE_BLINK_PERIOD_AMBIENT_MS : FACE_BLINK_PERIOD_FOREGROUND_MS;
	const int64_t blink_duration_ms =
		blink_profile_is_sleepy(profile) ? FACE_BLINK_DURATION_SLEEPY_MS :
						  FACE_BLINK_DURATION_DEFAULT_MS;

	if (blink_profile_is_disabled(profile)) {
		return false;
	}

	if (reaction_id == KERFUR_FACE_REACTION_REACTION_WAKE_BLINK) {
		return true;
	}

	if (reaction_has_visual_activity(reaction_id)) {
		return false;
	}

	return ((now_ms + ((int64_t)state->arousal * 31LL)) % blink_period_ms) < blink_duration_ms;
}

static int16_t smooth_axis(int16_t current, int16_t target, uint8_t speed)
{
	int delta = target - current;
	int step;

	if (delta == 0) {
		return current;
	}

	step = (delta * MAX((int)speed, 1)) / 100;
	if (step == 0) {
		step = sign_i16((int16_t)delta);
	}

	return current + (int16_t)step;
}

static void resolve_ambient_pupil_drift(const struct kerfur_face_recipe *recipe,
					int64_t now_ms,
					int16_t *dx,
					int16_t *dy)
{
	uint8_t index;

	*dx = 0;
	*dy = 0;

	for (index = 0U; index < recipe->ambient_motion_count; index++) {
		const struct kerfur_face_micro_animation *anim =
			kerfur_face_micro_anim_get(recipe->ambient_motion[index]);
		int64_t phase;
		int64_t quarter;

		if (anim->kind != KERFUR_FACE_MICRO_ANIM_KIND_PUPIL_MOTION ||
		    anim->duration_ms == 0U) {
			continue;
		}

		phase = now_ms % anim->duration_ms;
		quarter = MAX(1, anim->duration_ms / 4);

		if (phase < quarter) {
			*dx += anim->radius_x;
		} else if (phase < (2 * quarter)) {
			*dy += anim->radius_y;
		} else if (phase < (3 * quarter)) {
			*dx -= anim->radius_x;
		} else {
			*dy -= anim->radius_y;
		}
	}
}

static bool zone_is_available(const struct kerfur_face_asset_metadata *eye_asset)
{
	return (eye_asset != NULL) &&
	       (eye_asset->pupil_zone.shape != KERFUR_FACE_PUPIL_ZONE_NONE);
}

static void clamp_eye_offset_to_zone(const struct kerfur_face_eye_zone *zone,
				     int16_t *dx,
				     int16_t *dy)
{
	int16_t max_x = MAX(0, zone->rx - zone->margin);
	int16_t max_y = MAX(0, zone->ry - zone->margin);

	*dx = clamp_s16(*dx, -max_x, max_x);
	*dy = clamp_s16(*dy, -max_y, max_y);

	if (zone->shape != KERFUR_FACE_PUPIL_ZONE_ELLIPSE || max_x == 0 || max_y == 0) {
		return;
	}

	while (((*dx * *dx * max_y * max_y) + (*dy * *dy * max_x * max_x)) >
	       (max_x * max_x * max_y * max_y)) {
		if ((abs_i16(*dx) >= abs_i16(*dy)) && (*dx != 0)) {
			*dx -= sign_i16(*dx);
		} else if (*dy != 0) {
			*dy -= sign_i16(*dy);
		} else {
			break;
		}
	}
}

static void resolve_pupil_offsets(const struct kerfur_face_asset_metadata *eye_asset,
				  int16_t look_x,
				  int16_t look_y,
				  uint8_t scale_x,
				  uint8_t scale_y,
				  int16_t ambient_dx,
				  int16_t ambient_dy,
				  int16_t *out_dx,
				  int16_t *out_dy)
{
	const struct kerfur_face_eye_zone *zone = &eye_asset->pupil_zone;
	int16_t max_x = MAX(0, zone->rx - zone->margin);
	int16_t max_y = MAX(0, zone->ry - zone->margin);
	int16_t scaled_x = clamp_s16((look_x * scale_x) / 100, -100, 100);
	int16_t scaled_y = clamp_s16((look_y * scale_y) / 100, -100, 100);
	int16_t dx = percent_to_offset(scaled_x, max_x);
	int16_t dy = percent_to_offset(scaled_y, max_y);

	dx += ambient_dx;
	dy += ambient_dy;

	clamp_eye_offset_to_zone(zone, &dx, &dy);
	*out_dx = dx;
	*out_dy = dy;
}

static enum face_runtime_dynamic_reason resolve_dynamic_reason(
	const struct kerfur_face_recipe *recipe,
	const struct kerfur_face_reaction *reaction,
	const struct pet_state *state,
	int64_t now_ms,
	bool special_eye_mode_active,
	bool blink_active,
	bool has_zone,
	bool has_pupil)
{
	if (!has_pupil) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_NO_PUPIL;
	}
	if (state->dynamic_pupils_forced_disabled) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_FORCED_OFF;
	}
	if (state->battery_critical ||
	    (state->battery_low && !state->charging &&
	     (state->current_display_state != DISPLAY_FOREGROUND))) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_BATTERY_SAVE;
	}
	if (blink_active) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_BLINK;
	}
	if (reaction->has_allow_dynamic_pupils_override && !reaction->allow_dynamic_pupils) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_REACTION;
	}
	if (!reaction->has_allow_dynamic_pupils_override && !recipe->allow_dynamic_pupils &&
	    special_eye_mode_active) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_SPECIAL;
	}
	if (!reaction->has_allow_dynamic_pupils_override && !recipe->allow_dynamic_pupils) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_RECIPE;
	}
	if (!has_zone) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_NO_ZONE;
	}
	if (!state->in_hand || (state->in_hand_confidence < 45U)) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND;
	}
	if (state->walking_active || (state->walking_confidence >= 70U)) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_WALKING;
	}
	if ((state->last_rough_event_timestamp_ms > 0LL) &&
	    ((now_ms - state->last_rough_event_timestamp_ms) < 1200LL)) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_ROUGH;
	}
	if (state->look_confidence < 35U) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_LOW_CONFIDENCE;
	}

	return FACE_RUNTIME_DYNAMIC_ALLOWED;
}

static const struct kerfur_face_recipe *resolve_recipe_and_reaction(
	const struct pet_state *state,
	enum kerfur_face_recipe_id *recipe_id,
	enum kerfur_face_reaction_id *reaction_id,
	const struct kerfur_face_reaction **reaction_out)
{
	const struct kerfur_face_reaction *reaction;
	enum kerfur_face_recipe_id resolved_recipe_id;

	*reaction_id = sanitize_reaction_id(state->current_reaction);
	reaction = kerfur_face_reaction_get(*reaction_id);

	resolved_recipe_id = sanitize_recipe_id(state->current_expression);
	if ((reaction->compose_mode == KERFUR_FACE_REACTION_COMPOSE_REBASE) &&
	    (reaction->base_expression >= PET_EXPR_CALM) &&
	    (reaction->base_expression <= PET_EXPR_ASLEEP)) {
		resolved_recipe_id = (enum kerfur_face_recipe_id)reaction->base_expression;
	}

	*recipe_id = resolved_recipe_id;
	*reaction_out = reaction;
	return kerfur_face_recipe_get(resolved_recipe_id);
}

static uint8_t append_effect(struct face_runtime_plan *plan,
			     enum kerfur_face_asset_id asset_id,
			     struct kerfur_face_point point)
{
	if ((asset_id == KERFUR_FACE_ASSET_NONE) || (plan->effect_count >= KERFUR_FACE_MAX_EFFECTS)) {
		return plan->effect_count;
	}

	plan->effects[plan->effect_count].asset_id = asset_id;
	plan->effects[plan->effect_count].position = point;
	plan->effect_count++;
	return plan->effect_count;
}

static void resolve_effects(struct face_runtime_plan *plan,
			    const struct kerfur_face_recipe *recipe,
			    const struct kerfur_face_reaction *reaction)
{
	uint8_t index;
	struct kerfur_face_point effect_base = plan->layout.effect;

	plan->effect_count = 0U;

	for (index = 0U; index < recipe->default_effect_count; index++) {
		(void)append_effect(plan, recipe->default_effects[index], effect_base);
	}

	for (index = 0U; index < reaction->temporary_effect_count; index++) {
		struct kerfur_face_point point = effect_base;
		const struct kerfur_face_effect_instance *effect = &reaction->temporary_effects[index];

		if (effect->has_absolute_position) {
			point.x = effect->x;
			point.y = effect->y;
		}
		point.x += effect->dx;
		point.y += effect->dy;

		(void)append_effect(plan, effect->asset_id, point);
	}

	if (reaction->micro_animation != KERFUR_FACE_MICRO_ANIM_NONE) {
		const struct kerfur_face_micro_animation *anim =
			kerfur_face_micro_anim_get(reaction->micro_animation);

		if ((anim->kind == KERFUR_FACE_MICRO_ANIM_KIND_EFFECT_SPAWN) &&
		    (anim->effect_asset_id != KERFUR_FACE_ASSET_NONE)) {
			struct kerfur_face_point point = effect_base;

			point.x += anim->offset_x;
			point.y += anim->offset_y;
			(void)append_effect(plan, anim->effect_asset_id, point);
		}
	}
}

static void resolve_look_target(const struct kerfur_face_recipe *recipe,
				const struct kerfur_face_reaction *reaction,
				const struct pet_state *state,
				int16_t *target_x,
				int16_t *target_y)
{
	int16_t desired_x = recipe->base_look_x + reaction->look_offset_x;
	int16_t desired_y = recipe->base_look_y + reaction->look_offset_y;

	if (reaction->micro_animation != KERFUR_FACE_MICRO_ANIM_NONE) {
		const struct kerfur_face_micro_animation *anim =
			kerfur_face_micro_anim_get(reaction->micro_animation);

		if (anim->kind == KERFUR_FACE_MICRO_ANIM_KIND_LOOK_TARGET) {
			*target_x = clamp_s16(desired_x + anim->target_x, -100, 100);
			*target_y = clamp_s16(desired_y + anim->target_y, -100, 100);
			return;
		}
	}

	if (state->look_confidence > 0U) {
		desired_x += state->look_target_x;
		desired_y += state->look_target_y;
	}

	*target_x = clamp_s16(desired_x, -100, 100);
	*target_y = clamp_s16(desired_y, -100, 100);
}

static void log_plan(const struct face_runtime_plan *plan)
{
	uint8_t index;

	LOG_INF("Face plan recipe=%s reaction=%s indicator=%s overlay=%s blink=%d dynamic=%d reason=%s look_t=%d,%d look_r=%d,%d pupils_l=%d,%d pupils_r=%d,%d special=%d effects=%u",
		kerfur_face_recipe_name(plan->recipe_id),
		kerfur_face_reaction_name(plan->reaction_id),
		kerfur_face_indicator_name(plan->indicator_id),
		kerfur_face_overlay_name(plan->overlay_id),
		(plan->blink_left_active || plan->blink_right_active) ? 1 : 0,
		plan->dynamic_pupils_allowed ? 1 : 0,
		face_runtime_dynamic_reason_str(plan->dynamic_reason),
		plan->look_target_x,
		plan->look_target_y,
		plan->look_render_x,
		plan->look_render_y,
		plan->left_pupil_offset_x,
		plan->left_pupil_offset_y,
		plan->right_pupil_offset_x,
		plan->right_pupil_offset_y,
		plan->special_eye_mode_active ? 1 : 0,
		plan->effect_count);
	LOG_INF("Face assets eye_l=%s eye_r=%s pupil_l=%s pupil_r=%s brow_l=%s brow_r=%s mouth=%s whiskers=%s indicator=%s overlay=%s",
		kerfur_face_asset_name(plan->left_eye_white),
		kerfur_face_asset_name(plan->right_eye_white),
		kerfur_face_asset_name(plan->left_eyeball),
		kerfur_face_asset_name(plan->right_eyeball),
		kerfur_face_asset_name(plan->left_brow),
		kerfur_face_asset_name(plan->right_brow),
		kerfur_face_asset_name(plan->mouth),
		kerfur_face_asset_name(plan->whiskers),
		kerfur_face_asset_name(plan->indicator_asset),
		kerfur_face_asset_name(plan->overlay_asset));
	LOG_INF("Face coords eye_l=%d,%d eye_r=%d,%d brow_l=%d,%d brow_r=%d,%d mouth=%d,%d whisk_l=%d,%d whisk_r=%d,%d indicator=%d,%d overlay=%d,%d effect=%d,%d",
		plan->layout.left_eye_white.x,
		plan->layout.left_eye_white.y,
		plan->layout.right_eye_white.x,
		plan->layout.right_eye_white.y,
		plan->layout.left_brow.x,
		plan->layout.left_brow.y,
		plan->layout.right_brow.x,
		plan->layout.right_brow.y,
		plan->layout.mouth.x,
		plan->layout.mouth.y,
		plan->layout.left_whisker.x,
		plan->layout.left_whisker.y,
		plan->layout.right_whisker.x,
		plan->layout.right_whisker.y,
		plan->layout.indicator.x,
		plan->layout.indicator.y,
		plan->layout.overlay.x,
		plan->layout.overlay.y,
		plan->layout.effect.x,
		plan->layout.effect.y);

	for (index = 0U; index < plan->effect_count; index++) {
		LOG_INF("Face effect[%u] asset=%s pos=%d,%d",
			index,
			kerfur_face_asset_name(plan->effects[index].asset_id),
			plan->effects[index].position.x,
			plan->effects[index].position.y);
	}
}

void face_runtime_init(struct face_runtime_state *runtime)
{
	if (runtime == NULL) {
		return;
	}

	(void)memset(runtime, 0, sizeof(*runtime));
	runtime->current_expression_id = KERFUR_FACE_RECIPE_PET_EXPR_CALM;
	runtime->current_reaction_id = KERFUR_FACE_REACTION_REACTION_NONE;
	runtime->current_indicator_id = KERFUR_FACE_INDICATOR_NONE;
	runtime->current_overlay_id = KERFUR_FACE_OVERLAY_NONE;
	runtime->battery_percent = -1;
	runtime->plan.recipe_id = KERFUR_FACE_RECIPE_PET_EXPR_CALM;
	runtime->plan.reaction_id = KERFUR_FACE_REACTION_REACTION_NONE;
}

const struct face_runtime_plan *face_runtime_step(struct face_runtime_state *runtime,
						  struct pet_state *state,
						  int64_t now_ms,
						  bool ambient,
						  bool debug_dump_requested)
{
	if ((runtime == NULL) || (state == NULL)) {
		return NULL;
	}

	const struct kerfur_face_reaction *reaction;
	const struct kerfur_face_recipe *recipe;
	const struct kerfur_face_indicator_def *indicator_def;
	const struct kerfur_face_overlay_def *overlay_def;
	const struct kerfur_face_asset_metadata *left_eye_asset;
	const struct kerfur_face_asset_metadata *right_eye_asset;
	enum kerfur_face_recipe_id recipe_id;
	enum kerfur_face_reaction_id reaction_id;
	enum kerfur_face_blink_profile_id blink_profile_id;
	enum face_runtime_dynamic_reason dynamic_reason;
	enum kerfur_face_recipe_id previous_recipe_id = runtime->current_expression_id;
	enum kerfur_face_reaction_id previous_reaction_id = runtime->current_reaction_id;
	enum kerfur_face_indicator_id previous_indicator_id = runtime->current_indicator_id;
	enum kerfur_face_overlay_id previous_overlay_id = runtime->current_overlay_id;
	struct face_runtime_plan plan;
	int16_t target_x;
	int16_t target_y;
	int16_t ambient_dx = 0;
	int16_t ambient_dy = 0;
	bool special_eye_mode_active;
	bool blink_active;
	bool has_zone;
	bool has_pupil;
	bool dynamic_allowed;
	uint8_t motion_confidence;
	uint8_t motion_scale;
	uint8_t motion_speed;
	bool should_log;

	recipe = resolve_recipe_and_reaction(state, &recipe_id, &reaction_id, &reaction);
	blink_profile_id = resolve_blink_profile(recipe, reaction);

	(void)memset(&plan, 0, sizeof(plan));
	plan.recipe_id = recipe_id;
	plan.reaction_id = reaction_id;
	plan.left_eye_white = choose_asset(recipe->left_eye_white, reaction->left_eye_white);
	plan.right_eye_white = choose_asset(recipe->right_eye_white, reaction->right_eye_white);
	plan.blink_left_eye_white = recipe->blink_left_eye_white;
	plan.blink_right_eye_white = recipe->blink_right_eye_white;
	plan.left_eyeball = choose_asset(recipe->left_eyeball, reaction->left_eyeball);
	plan.right_eyeball = choose_asset(recipe->right_eyeball, reaction->right_eyeball);
	plan.left_brow = choose_asset(recipe->left_brow, reaction->left_brow);
	plan.right_brow = choose_asset(recipe->right_brow, reaction->right_brow);
	plan.mouth = choose_asset(recipe->mouth, reaction->mouth);
	plan.whiskers = choose_asset(recipe->whiskers, reaction->whiskers);
	plan.indicator_id = resolve_indicator(state, recipe, reaction);
	plan.overlay_id = resolve_overlay(state, recipe, reaction);
	plan.blink_profile_id = blink_profile_id;
	plan.layout = resolve_layout(recipe, reaction);

	indicator_def = kerfur_face_indicator_get(plan.indicator_id);
	overlay_def = kerfur_face_overlay_get(plan.overlay_id);
	plan.indicator_asset = indicator_def->asset_id;
	plan.overlay_asset = overlay_def->asset_id;

	left_eye_asset = kerfur_face_asset_get(plan.left_eye_white);
	right_eye_asset = kerfur_face_asset_get(plan.right_eye_white);
	special_eye_mode_active =
		recipe->special_eye_mode ||
		((left_eye_asset->flags & KERFUR_FACE_ASSET_FLAG_SPECIAL_MODE) != 0U) ||
		((right_eye_asset->flags & KERFUR_FACE_ASSET_FLAG_SPECIAL_MODE) != 0U);
	blink_active = should_blink(state, reaction_id, blink_profile_id, now_ms, ambient);

	resolve_look_target(recipe, reaction, state, &target_x, &target_y);

	if ((recipe->dead_zone_strength > 0U) &&
	    (abs_i16(target_x) <= recipe->dead_zone_strength)) {
		target_x = 0;
	}
	if ((recipe->dead_zone_strength > 0U) &&
	    (abs_i16(target_y) <= recipe->dead_zone_strength)) {
		target_y = 0;
	}

	has_zone = zone_is_available(left_eye_asset) || zone_is_available(right_eye_asset);
	has_pupil = (plan.left_eyeball != KERFUR_FACE_ASSET_NONE) ||
		    (plan.right_eyeball != KERFUR_FACE_ASSET_NONE);
	dynamic_reason = resolve_dynamic_reason(recipe, reaction, state, now_ms,
						 special_eye_mode_active, blink_active, has_zone,
						 has_pupil);
	dynamic_allowed = dynamic_reason == FACE_RUNTIME_DYNAMIC_ALLOWED;

	motion_confidence = MIN(state->look_confidence, state->in_hand_confidence);
	if (state->walking_confidence > 0U) {
		motion_confidence = (uint8_t)MIN(motion_confidence,
						(uint8_t)MAX(0, 100 - state->walking_confidence));
	}
	if (state->battery_low) {
		motion_confidence = (uint8_t)MIN(motion_confidence, 60U);
	}
	motion_scale = motion_scale_for_recipe(recipe_id);
	motion_speed = motion_speed_for_recipe(recipe_id);
	if (motion_scale == 0U) {
		motion_confidence = 0U;
	}

	if (!dynamic_allowed) {
		switch (dynamic_reason) {
		case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_WALKING:
			target_x /= 5;
			target_y /= 5;
			motion_speed = MAX(motion_speed, 35U);
			break;
		case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_ROUGH:
			target_x = 0;
			target_y = 0;
			motion_confidence = 0U;
			motion_speed = MAX(motion_speed, 55U);
			break;
		case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_LOW_CONFIDENCE:
		case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND:
			target_x /= 3;
			target_y /= 3;
			motion_confidence = (uint8_t)(motion_confidence / 2U);
			motion_speed = MAX(motion_speed, 25U);
			break;
		case FACE_RUNTIME_DYNAMIC_DISABLED_BATTERY_SAVE:
			target_x /= 4;
			target_y /= 4;
			motion_confidence = (uint8_t)(motion_confidence / 3U);
			motion_speed = MAX(motion_speed, 18U);
			break;
		case FACE_RUNTIME_DYNAMIC_DISABLED_FORCED_OFF:
			target_x = 0;
			target_y = 0;
			motion_confidence = 0U;
			motion_speed = MAX(motion_speed, 30U);
			break;
		default:
			break;
		}
	} else {
		target_x = (target_x * motion_scale) / 100;
		target_y = (target_y * motion_scale) / 100;
		motion_speed = MAX(8U, (uint8_t)((motion_speed * MAX(motion_confidence, 15U)) / 100U));
	}

	if ((runtime->last_step_ms == 0) || (now_ms < runtime->last_step_ms)) {
		runtime->look_render_x = target_x;
		runtime->look_render_y = target_y;
	} else {
		runtime->look_render_x = smooth_axis(runtime->look_render_x, target_x, motion_speed);
		runtime->look_render_y = smooth_axis(runtime->look_render_y, target_y, motion_speed);
	}

	resolve_ambient_pupil_drift(recipe, now_ms, &ambient_dx, &ambient_dy);

	if (dynamic_allowed) {
		if (zone_is_available(left_eye_asset)) {
			resolve_pupil_offsets(left_eye_asset, runtime->look_render_x, runtime->look_render_y,
					      recipe->dynamic_pupil_scale_x,
					      recipe->dynamic_pupil_scale_y, ambient_dx, ambient_dy,
					      &plan.left_pupil_offset_x, &plan.left_pupil_offset_y);
		}
		if (zone_is_available(right_eye_asset)) {
			resolve_pupil_offsets(right_eye_asset, runtime->look_render_x, runtime->look_render_y,
					      recipe->dynamic_pupil_scale_x,
					      recipe->dynamic_pupil_scale_y, ambient_dx, ambient_dy,
					      &plan.right_pupil_offset_x, &plan.right_pupil_offset_y);
		}
	}

	resolve_effects(&plan, recipe, reaction);

	plan.look_target_x = target_x;
	plan.look_target_y = target_y;
	plan.look_render_x = runtime->look_render_x;
	plan.look_render_y = runtime->look_render_y;
	plan.dynamic_pupils_allowed = dynamic_allowed;
	plan.special_eye_mode_active = special_eye_mode_active;
	plan.blink_left_active = blink_active;
	plan.blink_right_active = blink_active;
	plan.dynamic_reason = dynamic_reason;

	runtime->current_expression_id = recipe_id;
	runtime->current_reaction_id = reaction_id;
	runtime->current_indicator_id = plan.indicator_id;
	runtime->current_overlay_id = plan.overlay_id;
	runtime->look_target_x = target_x;
	runtime->look_target_y = target_y;
	runtime->look_render_x = plan.look_render_x;
	runtime->look_render_y = plan.look_render_y;
	runtime->look_confidence = state->look_confidence;
	runtime->dynamic_pupils_allowed = dynamic_allowed;
	runtime->special_eye_mode_active = special_eye_mode_active;
	runtime->blink_active = blink_active;
	runtime->in_hand = state->in_hand;
	runtime->pickup_confidence = state->pickup_confidence;
	runtime->in_hand_confidence = state->in_hand_confidence;
	runtime->walking_confidence = state->walking_confidence;
	runtime->battery_percent = state->battery_percent;
	runtime->battery_percent_known = state->battery_percent_known;
	runtime->last_step_ms = now_ms;
	runtime->plan = plan;

	state->look_render_x = runtime->look_render_x;
	state->look_render_y = runtime->look_render_y;

	should_log = IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG_VERBOSE) || debug_dump_requested ||
		     (previous_recipe_id != runtime->plan.recipe_id) ||
		     (previous_reaction_id != runtime->plan.reaction_id) ||
		     (previous_indicator_id != runtime->plan.indicator_id) ||
		     (previous_overlay_id != runtime->plan.overlay_id);

	if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG) && should_log) {
		log_plan(&runtime->plan);
	}

	return &runtime->plan;
}

const char *face_runtime_dynamic_reason_str(enum face_runtime_dynamic_reason reason)
{
	switch (reason) {
	case FACE_RUNTIME_DYNAMIC_ALLOWED:
		return "allowed";
	case FACE_RUNTIME_DYNAMIC_DISABLED_RECIPE:
		return "recipe_disabled";
	case FACE_RUNTIME_DYNAMIC_DISABLED_REACTION:
		return "reaction_disabled";
	case FACE_RUNTIME_DYNAMIC_DISABLED_BLINK:
		return "blink";
	case FACE_RUNTIME_DYNAMIC_DISABLED_NO_ZONE:
		return "no_zone";
	case FACE_RUNTIME_DYNAMIC_DISABLED_SPECIAL:
		return "special_mode";
	case FACE_RUNTIME_DYNAMIC_DISABLED_NO_PUPIL:
		return "no_pupil";
	case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_WALKING:
		return "walking";
	case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_ROUGH:
		return "rough";
	case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_LOW_CONFIDENCE:
		return "low_confidence";
	case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND:
		return "not_in_hand";
	case FACE_RUNTIME_DYNAMIC_DISABLED_BATTERY_SAVE:
		return "battery_save";
	case FACE_RUNTIME_DYNAMIC_DISABLED_FORCED_OFF:
		return "forced_off";
	default:
		return "unknown";
	}
}
