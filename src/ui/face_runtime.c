#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
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

/*
 * Indicator render priority — first entries are drawn leftmost.
 * Keep this list aligned with assets/face/kerfur_faces.json
 * `ui_rules.indicator_priority`.
 */
static const enum kerfur_face_indicator_id g_indicator_priority[] = {
	KERFUR_FACE_INDICATOR_ICON_QUESTION,
	KERFUR_FACE_INDICATOR_ICON_HEART_FILLED,
	KERFUR_FACE_INDICATOR_ICON_HEART_OUTLINE,
	KERFUR_FACE_INDICATOR_ICON_BT,
	KERFUR_FACE_INDICATOR_ICON_X,
};

static void candidate_add(enum kerfur_face_indicator_id *candidates, uint8_t *count,
			  enum kerfur_face_indicator_id id)
{
	uint8_t i;

	if (id == KERFUR_FACE_INDICATOR_NONE) {
		return;
	}

	for (i = 0U; i < *count; i++) {
		if (candidates[i] == id) {
			return;
		}
	}

	if (*count >= KERFUR_FACE_INDICATOR_COUNT) {
		return;
	}

	candidates[(*count)++] = id;
}

static uint8_t resolve_indicators(const struct pet_state *state,
				  const struct kerfur_face_recipe *recipe,
				  const struct kerfur_face_reaction *reaction,
				  int64_t now_ms,
				  enum kerfur_face_indicator_id *out_ids,
				  uint8_t out_capacity)
{
	enum kerfur_face_indicator_id candidates[KERFUR_FACE_INDICATOR_COUNT];
	uint8_t candidate_count = 0U;
	bool explicit_override;
	enum kerfur_face_indicator_id explicit_id;
	enum kerfur_face_indicator_id social_id;
	uint8_t out_count = 0U;
	uint8_t i;

	/* 1. reaction override (always wins as the leftmost slot when present) */
	if (reaction->indicator != KERFUR_FACE_INDICATOR_NONE) {
		candidate_add(candidates, &candidate_count, reaction->indicator);
	}

	/* 2. explicit pet_state.current_indicator */
	explicit_id = sanitize_indicator_id(state->current_indicator, &explicit_override);
	if (explicit_override && (explicit_id != KERFUR_FACE_INDICATOR_NONE)) {
		candidate_add(candidates, &candidate_count, explicit_id);
	}

	/* 3. transient social indicator (peer/encounter) while its TTL is alive */
	if ((state->social_indicator > 0) &&
	    (state->social_indicator < KERFUR_FACE_INDICATOR_COUNT) &&
	    (state->social_indicator_until_ms > now_ms)) {
		social_id = (enum kerfur_face_indicator_id)state->social_indicator;
		candidate_add(candidates, &candidate_count, social_id);
	}

	/* 4. recipe default */
	candidate_add(candidates, &candidate_count, recipe->default_indicator);

	/* 5. BLE-connected fallback (companion phone) */
	if (state->ble_connected) {
		candidate_add(candidates, &candidate_count, KERFUR_FACE_INDICATOR_ICON_BT);
	}

	/* Emit in priority order, clipped to capacity. */
	for (i = 0U; i < ARRAY_SIZE(g_indicator_priority); i++) {
		uint8_t j;
		enum kerfur_face_indicator_id pri = g_indicator_priority[i];

		for (j = 0U; j < candidate_count; j++) {
			if (candidates[j] == pri) {
				if (out_count < out_capacity) {
					out_ids[out_count++] = pri;
				}
				break;
			}
		}

		if (out_count >= out_capacity) {
			break;
		}
	}

	return out_count;
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

static bool blink_profile_is_sleepy(enum kerfur_face_blink_profile_id profile)
{
	return profile == KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_SLEEPY;
}

static bool blink_profile_is_disabled(enum kerfur_face_blink_profile_id profile)
{
	return profile == KERFUR_FACE_BLINK_PROFILE_BLINK_DISABLED;
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
	if (!state->in_hand && (state->in_hand_confidence < 15U) &&
	    (state->look_confidence < 5U)) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND;
	}
	if ((state->last_rough_event_timestamp_ms > 0LL) &&
	    ((now_ms - state->last_rough_event_timestamp_ms) < 800LL)) {
		return FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_ROUGH;
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

	LOG_INF("Face channels eye_open=%u brow_dy=%d,%d mouth_dy=%d whisk_dy=%d,%d tear_dy=%d transition=%d per_eye=%d l_eye_open=%u r_eye_open=%u",
		plan->eye_openness,
		plan->left_brow_dy,
		plan->right_brow_dy,
		plan->mouth_dy,
		plan->left_whisker_dy,
		plan->right_whisker_dy,
		plan->tear_drift_dy,
		plan->transition_active ? 1 : 0,
		plan->per_eye_openness ? 1 : 0,
		plan->left_eye_openness,
		plan->right_eye_openness);

	for (index = 0U; index < plan->effect_count; index++) {
		LOG_INF("Face effect[%u] asset=%s pos=%d,%d",
			index,
			kerfur_face_asset_name(plan->effects[index].asset_id),
			plan->effects[index].position.x,
			plan->effects[index].position.y);
	}
}

#define FACE_TRANSITION_SPEED_DEFAULT 40
#define FACE_EYE_OPENNESS_DEFAULT    100
#define FACE_BLINK_CLOSE_SPEED       65
#define FACE_BLINK_OPEN_SPEED        35
#define FACE_BLINK_CLOSE_SPEED_SLEEPY 45
#define FACE_BLINK_OPEN_SPEED_SLEEPY  25

struct whisker_wiggle_frame {
	int8_t dy;
	uint16_t duration_ms;
};

static const struct whisker_wiggle_frame g_whisker_wiggle[] = {
	{ .dy =  2, .duration_ms = 120 },
	{ .dy = -2, .duration_ms = 120 },
	{ .dy =  2, .duration_ms = 120 },
	{ .dy = -1, .duration_ms = 170 },
	{ .dy =  1, .duration_ms = 170 },
	{ .dy =  0, .duration_ms = 260 },
};

static int16_t evaluate_whisker_wiggle(int64_t start_ms, int64_t now_ms)
{
	int64_t elapsed = now_ms - start_ms;
	int64_t cursor = 0;
	uint8_t i;

	if ((start_ms <= 0) || (elapsed < 0)) {
		return 0;
	}

	for (i = 0U; i < ARRAY_SIZE(g_whisker_wiggle); i++) {
		cursor += g_whisker_wiggle[i].duration_ms;
		if (elapsed < cursor) {
			return g_whisker_wiggle[i].dy;
		}
	}

	return 0; /* animation finished */
}

/* --- Stage 5: organic micro-life helpers --- */

static uint32_t face_rand(uint32_t span)
{
	return (span == 0U) ? 0U : (sys_rand32_get() % span);
}

/* Emotion-aware blink cadence: alert pets blink noticeably more often,
 * sleepy pets slower (their blinks are also longer); the interval is
 * always jittered so the rhythm never feels mechanical. */
static int64_t next_blink_delay_ms(const struct pet_state *state, bool ambient)
{
	int64_t period = ambient ? FACE_BLINK_PERIOD_AMBIENT_MS
				 : FACE_BLINK_PERIOD_FOREGROUND_MS;

	if ((state->arousal > 55) || (state->stress > 45)) {
		period = (period * 6) / 10;
	} else if (state->sleepiness > 65) {
		period = (period * 15) / 10;
	}

	/* ±40% jitter */
	return (period * (int64_t)(60U + face_rand(81U))) / 100;
}

static void update_blink_state(struct face_runtime_state *runtime,
			       const struct kerfur_face_recipe *recipe,
			       enum kerfur_face_blink_profile_id profile,
			       const struct pet_state *state,
			       int64_t now_ms,
			       bool ambient)
{
	const int64_t blink_duration =
		blink_profile_is_sleepy(profile) ? FACE_BLINK_DURATION_SLEEPY_MS
						: FACE_BLINK_DURATION_DEFAULT_MS;
	int16_t base_openness =
		clamp_s16((int16_t)recipe->base_eye_openness + runtime->ctx_openness_bias, 0, 100);

	if (blink_profile_is_disabled(profile)) {
		runtime->tgt_eye_openness = base_openness;
		return;
	}

	/* Schedule first blink */
	if (runtime->blink_next_ms == 0) {
		runtime->blink_next_ms = now_ms + next_blink_delay_ms(state, ambient);
		runtime->tgt_eye_openness = base_openness;
		return;
	}

	/* Blink in progress: closing phase */
	if (runtime->blink_descending) {
		runtime->tgt_eye_openness = 0;
		if ((now_ms - runtime->blink_phase_start_ms) >= blink_duration) {
			runtime->blink_descending = false;
			runtime->tgt_eye_openness = base_openness;
			if (runtime->blink_double_pending) {
				/* Quick second blink lands shortly after. */
				runtime->blink_double_pending = false;
				runtime->blink_next_ms =
					now_ms + 350LL + (int64_t)face_rand(250U);
			} else {
				runtime->blink_next_ms =
					now_ms + next_blink_delay_ms(state, ambient);
			}
		}
		return;
	}

	/* Check if it's time to blink */
	if (now_ms >= runtime->blink_next_ms) {
		runtime->blink_descending = true;
		runtime->blink_phase_start_ms = now_ms;
		runtime->tgt_eye_openness = 0;
		/* Occasional double blink, more likely when alert. */
		if (!blink_profile_is_sleepy(profile)) {
			const uint32_t chance =
				((state->arousal > 55) || (state->stress > 40)) ? 22U : 10U;

			runtime->blink_double_pending = face_rand(100U) < chance;
		}
		return;
	}

	/* Not blinking — hold base openness */
	runtime->tgt_eye_openness = base_openness;
}

/* Wandering idle gaze: when there is no real gaze input and nothing
 * else is happening, the eyes occasionally drift to a random nearby
 * point, hold it for a moment, then come back to rest. Sleepy faces
 * mostly skip their turn; ambient mode wanders less often. */

#define FACE_WANDER_RANGE 35

static void update_wander(struct face_runtime_state *runtime,
			  const struct pet_state *state,
			  enum kerfur_face_reaction_id reaction_id,
			  enum kerfur_face_blink_profile_id profile,
			  bool ambient,
			  int64_t now_ms)
{
	const bool quiet = (reaction_id == KERFUR_FACE_REACTION_REACTION_NONE) &&
			   !state->in_hand &&
			   (state->look_confidence < 5U) &&
			   !state->battery_low && !state->battery_critical;

	if (!quiet) {
		runtime->wander_active = false;
		runtime->wander_x = 0;
		runtime->wander_y = 0;
		/* Re-arm so the next quiet stretch doesn't fire instantly. */
		runtime->wander_next_ms = now_ms + 4000LL + (int64_t)face_rand(6000U);
		return;
	}

	if (runtime->wander_next_ms == 0LL) {
		runtime->wander_next_ms = now_ms + 4000LL + (int64_t)face_rand(8000U);
		return;
	}

	if (runtime->wander_active) {
		if (now_ms >= runtime->wander_hold_until_ms) {
			runtime->wander_active = false;
			runtime->wander_x = 0;
			runtime->wander_y = 0;
			runtime->wander_next_ms = now_ms +
				(ambient ? 12000LL : 6000LL) +
				(int64_t)face_rand(ambient ? 14000U : 10000U);
		}
		return;
	}

	if (now_ms < runtime->wander_next_ms) {
		return;
	}

	if (blink_profile_is_sleepy(profile) && (face_rand(100U) < 70U)) {
		runtime->wander_next_ms = now_ms + 9000LL + (int64_t)face_rand(9000U);
		return;
	}

	runtime->wander_x =
		(int16_t)((int)face_rand((2U * FACE_WANDER_RANGE) + 1U) - FACE_WANDER_RANGE);
	runtime->wander_y =
		(int16_t)((int)face_rand(FACE_WANDER_RANGE + 1U) - (FACE_WANDER_RANGE / 2));
	/* Avoid pointless 1–2 px micro-targets. */
	if ((abs_i16(runtime->wander_x) < 12) && (abs_i16(runtime->wander_y) < 10)) {
		runtime->wander_x = (runtime->wander_x >= 0) ? 18 : -18;
	}
	runtime->wander_active = true;
	runtime->wander_hold_until_ms = now_ms + 1200LL + (int64_t)face_rand(1600U);
}

/* Slow breathing bob: a single 1 px mouth/whisker lift that appears only
 * briefly near the top of a long, slow cycle. Most of the time the mouth
 * and whiskers rest perfectly still, so the motion reads as a faint breath
 * rather than the constant bob it used to be. Sleepier = slower; it keeps
 * going while asleep — a sleeping pet that still breathes reads as alive. */
static int16_t breathing_offset(const struct pet_state *state, int64_t now_ms)
{
	const int64_t period_ms = (state->sleepiness > 65) ? 9000LL :
				  (state->arousal > 55) ? 5500LL : 7000LL;
	const int64_t phase = now_ms % period_ms;

	/* Lifted for only the last ~18% of each cycle. */
	return (phase < ((period_ms * 82) / 100)) ? 0 : 1;
}

static void detect_expression_transition(struct face_runtime_state *runtime,
					 enum kerfur_face_recipe_id new_recipe_id,
					 const struct kerfur_face_recipe *new_recipe,
					 int64_t now_ms)
{
	const struct kerfur_face_recipe *old_recipe;

	if (new_recipe_id == runtime->current_expression_id) {
		return;
	}

	old_recipe = kerfur_face_recipe_get(runtime->current_expression_id);

	runtime->ch_left_brow_dy =
		(int16_t)(old_recipe->layout.left_brow.y - new_recipe->layout.left_brow.y);
	runtime->ch_right_brow_dy =
		(int16_t)(old_recipe->layout.right_brow.y - new_recipe->layout.right_brow.y);
	runtime->ch_mouth_dy =
		(int16_t)(old_recipe->layout.mouth.y - new_recipe->layout.mouth.y);
	runtime->ch_left_whisker_dy =
		(int16_t)(old_recipe->layout.left_whisker.y - new_recipe->layout.left_whisker.y);
	runtime->ch_right_whisker_dy =
		(int16_t)(old_recipe->layout.right_whisker.y - new_recipe->layout.right_whisker.y);

	runtime->tgt_left_brow_dy = 0;
	runtime->tgt_right_brow_dy = 0;
	runtime->tgt_mouth_dy = 0;
	runtime->tgt_left_whisker_dy = 0;
	runtime->tgt_right_whisker_dy = 0;

	runtime->tgt_eye_openness = (int16_t)new_recipe->base_eye_openness;

	runtime->prev_expression_id = runtime->current_expression_id;
	runtime->transition_start_ms = now_ms;
	runtime->transition_active = true;
}

static void step_channels(struct face_runtime_state *runtime,
			  const struct kerfur_face_recipe *recipe,
			  int64_t now_ms)
{
	uint8_t speed = (recipe->transition_speed > 0U) ? recipe->transition_speed
							: FACE_TRANSITION_SPEED_DEFAULT;
	uint8_t eye_speed;
	bool all_settled;

	(void)now_ms;

	/* Bake context bias into the effective target so the smoothing pass
	 * and the settled check both compare against the same value. */
	const int16_t left_brow_target =
		runtime->tgt_left_brow_dy + runtime->ctx_brow_bias_dy;
	const int16_t right_brow_target =
		runtime->tgt_right_brow_dy + runtime->ctx_brow_bias_dy;
	const int16_t mouth_target =
		runtime->tgt_mouth_dy + runtime->ctx_mouth_bias_dy;

	runtime->ch_left_brow_dy =
		smooth_axis(runtime->ch_left_brow_dy, left_brow_target, speed);
	runtime->ch_right_brow_dy =
		smooth_axis(runtime->ch_right_brow_dy, right_brow_target, speed);
	runtime->ch_mouth_dy =
		smooth_axis(runtime->ch_mouth_dy, mouth_target, speed);
	runtime->ch_left_whisker_dy =
		smooth_axis(runtime->ch_left_whisker_dy, runtime->tgt_left_whisker_dy, speed);
	runtime->ch_right_whisker_dy =
		smooth_axis(runtime->ch_right_whisker_dy, runtime->tgt_right_whisker_dy, speed);

	if (runtime->blink_descending) {
		eye_speed = (recipe->blink_profile == KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_SLEEPY)
				? FACE_BLINK_CLOSE_SPEED_SLEEPY : FACE_BLINK_CLOSE_SPEED;
	} else {
		eye_speed = (recipe->blink_profile == KERFUR_FACE_BLINK_PROFILE_BLINK_LEGACY_SLEEPY)
				? FACE_BLINK_OPEN_SPEED_SLEEPY : FACE_BLINK_OPEN_SPEED;
	}
	runtime->ch_eye_openness =
		smooth_axis(runtime->ch_eye_openness, runtime->tgt_eye_openness, eye_speed);

	all_settled = (runtime->ch_left_brow_dy == left_brow_target) &&
		      (runtime->ch_right_brow_dy == right_brow_target) &&
		      (runtime->ch_mouth_dy == mouth_target) &&
		      (runtime->ch_left_whisker_dy == runtime->tgt_left_whisker_dy) &&
		      (runtime->ch_right_whisker_dy == runtime->tgt_right_whisker_dy) &&
		      (runtime->ch_eye_openness == runtime->tgt_eye_openness);

	if (all_settled && runtime->transition_active) {
		runtime->transition_active = false;
	}
}

/* --- Stage 3: Pupil swap policies --- */

static void resolve_pupil_swap(struct face_runtime_state *runtime,
			       const struct kerfur_face_recipe *recipe,
			       const struct kerfur_face_reaction *reaction,
			       struct face_runtime_plan *plan,
			       int64_t now_ms)
{
	enum face_pupil_swap_style style;
	bool eyeball_changed;

	/* Determine effective swap style: reaction override > recipe default */
	if (reaction->pupil_swap_override > 0U) {
		style = (enum face_pupil_swap_style)reaction->pupil_swap_override;
	} else {
		style = (enum face_pupil_swap_style)recipe->pupil_swap_style;
	}

	/* ON_BLINK hides the pupil change behind a blink — but a face whose
	 * blink profile is disabled (e.g. OVERSTIMULATED's spiral eyes) never
	 * closes, so the swap would wait forever and the new pupil would never
	 * appear. Fall back to a time-based settle so the swap always resolves. */
	if ((style == FACE_PUPIL_SWAP_ON_BLINK) &&
	    blink_profile_is_disabled(resolve_blink_profile(recipe, reaction))) {
		style = FACE_PUPIL_SWAP_SETTLE;
	}

	eyeball_changed =
		(plan->left_eyeball != runtime->plan.left_eyeball) ||
		(plan->right_eyeball != runtime->plan.right_eyeball);

	if (!eyeball_changed && !runtime->pupil_swap_waiting_for_blink) {
		return;
	}

	switch (style) {
	case FACE_PUPIL_SWAP_ON_BLINK:
		if (eyeball_changed && !runtime->pupil_swap_waiting_for_blink) {
			/* Defer the swap until eyes close */
			runtime->deferred_left_eyeball = plan->left_eyeball;
			runtime->deferred_right_eyeball = plan->right_eyeball;
			runtime->pupil_swap_waiting_for_blink = true;
			/* Keep current pupils for now */
			plan->left_eyeball = runtime->plan.left_eyeball;
			plan->right_eyeball = runtime->plan.right_eyeball;
		}
		if (runtime->pupil_swap_waiting_for_blink) {
			if (runtime->ch_eye_openness < 10) {
				/* Eyes are closed — do the swap */
				plan->left_eyeball = runtime->deferred_left_eyeball;
				plan->right_eyeball = runtime->deferred_right_eyeball;
				runtime->pupil_swap_waiting_for_blink = false;
			} else {
				plan->left_eyeball = runtime->plan.left_eyeball;
				plan->right_eyeball = runtime->plan.right_eyeball;
			}
		}
		break;

	case FACE_PUPIL_SWAP_SETTLE:
		if (eyeball_changed && !runtime->pupil_swap_waiting_for_blink) {
			runtime->deferred_left_eyeball = plan->left_eyeball;
			runtime->deferred_right_eyeball = plan->right_eyeball;
			runtime->pupil_swap_waiting_for_blink = true;
			runtime->pupil_settle_start_ms = now_ms;
			plan->left_eyeball = runtime->plan.left_eyeball;
			plan->right_eyeball = runtime->plan.right_eyeball;
		}
		if (runtime->pupil_swap_waiting_for_blink) {
			/* Wait for pupils to settle near center (~400ms) */
			if ((now_ms - runtime->pupil_settle_start_ms) >= 400LL) {
				plan->left_eyeball = runtime->deferred_left_eyeball;
				plan->right_eyeball = runtime->deferred_right_eyeball;
				runtime->pupil_swap_waiting_for_blink = false;
			} else {
				plan->left_eyeball = runtime->plan.left_eyeball;
				plan->right_eyeball = runtime->plan.right_eyeball;
			}
		}
		break;

	case FACE_PUPIL_SWAP_INSTANT:
	default:
		/* Immediate swap — already in plan */
		runtime->pupil_swap_waiting_for_blink = false;
		break;
	}
}

/* --- Stage 3: Reaction stagger --- */

static void resolve_reaction_stagger(struct face_runtime_state *runtime,
				     const struct kerfur_face_reaction *reaction,
				     struct face_runtime_plan *plan,
				     const struct kerfur_face_recipe *recipe,
				     int64_t now_ms)
{
	int64_t elapsed;

	if (reaction->stagger_delay_ms == 0U) {
		return;
	}

	/* Detect reaction start */
	if (plan->reaction_id != runtime->current_reaction_id) {
		runtime->reaction_enter_ms = now_ms;
		runtime->reaction_stagger_phase = 0U;
	}

	if (runtime->reaction_enter_ms <= 0) {
		return;
	}

	elapsed = now_ms - runtime->reaction_enter_ms;

	/* Phase 0: only eye assets applied (already in plan) */
	/* Phase 1: after 1x stagger_delay — apply brow assets */
	/* Phase 2: after 2x stagger_delay — apply mouth/whiskers */

	if (elapsed < reaction->stagger_delay_ms) {
		/* Phase 0: revert brows and mouth to base expression */
		plan->left_brow = recipe->left_brow;
		plan->right_brow = recipe->right_brow;
		plan->mouth = recipe->mouth;
		plan->whiskers = recipe->whiskers;
	} else if (elapsed < (int64_t)(2U * reaction->stagger_delay_ms)) {
		/* Phase 1: brows applied, mouth/whiskers still base */
		plan->mouth = recipe->mouth;
		plan->whiskers = recipe->whiskers;
	}
	/* Phase 2+: all reaction assets applied (already in plan) */
}

/* --- Stage 3: NOTIF_PING alternating wink --- */

#define NOTIF_WINK_TOGGLE_MS 500LL

static void apply_notif_ping_wink(struct face_runtime_state *runtime,
				  struct face_runtime_plan *plan,
				  int64_t now_ms)
{
	if (plan->reaction_id != KERFUR_FACE_REACTION_REACTION_NOTIF_PING) {
		runtime->notif_wink_left_closed = false;
		runtime->notif_wink_last_toggle_ms = 0;
		return;
	}

	if (runtime->notif_wink_last_toggle_ms == 0) {
		runtime->notif_wink_last_toggle_ms = now_ms;
		runtime->notif_wink_left_closed = true;
	}

	if ((now_ms - runtime->notif_wink_last_toggle_ms) >= NOTIF_WINK_TOGGLE_MS) {
		runtime->notif_wink_left_closed = !runtime->notif_wink_left_closed;
		runtime->notif_wink_last_toggle_ms = now_ms;
	}

	plan->per_eye_openness = true;
	plan->right_eye_openness = plan->eye_openness;
	plan->left_eye_openness = runtime->notif_wink_left_closed ? 0U : plan->eye_openness;
}

/* --- Stage 3: ANNOYED chaotic pupil jitter --- */

static void apply_annoyed_jitter(struct face_runtime_state *runtime,
				 struct face_runtime_plan *plan)
{
	if (plan->recipe_id != KERFUR_FACE_RECIPE_PET_EXPR_ANNOYED ||
	    !plan->dynamic_pupils_allowed) {
		runtime->jitter_offset_x = 0;
		runtime->jitter_offset_y = 0;
		return;
	}

	runtime->jitter_seed = (uint16_t)((runtime->jitter_seed * 25173U + 13849U) >> 1);
	runtime->jitter_offset_x = (int16_t)(((runtime->jitter_seed >> 0) & 3U) - 1);
	runtime->jitter_offset_y = (int16_t)(((runtime->jitter_seed >> 2) & 3U) - 1);

	plan->left_pupil_offset_x += runtime->jitter_offset_x;
	plan->left_pupil_offset_y += runtime->jitter_offset_y;
	plan->right_pupil_offset_x += runtime->jitter_offset_x;
	plan->right_pupil_offset_y += runtime->jitter_offset_y;
}

/* --- Stage 3: Tear drift --- */

#define TEAR_DRIFT_RESET_MS 3000LL

static void update_tear_drift(struct face_runtime_state *runtime,
			      const struct face_runtime_plan *plan,
			      int64_t now_ms)
{
	bool has_tears = false;
	uint8_t i;

	for (i = 0U; i < plan->effect_count; i++) {
		if (plan->effects[i].asset_id == KERFUR_FACE_ASSET_EFFECT_TEAR) {
			has_tears = true;
			break;
		}
	}

	if (!has_tears) {
		runtime->tear_drift_dy = 0;
		runtime->tear_last_spawn_ms = 0;
		return;
	}

	if (runtime->tear_last_spawn_ms == 0) {
		runtime->tear_last_spawn_ms = now_ms;
		runtime->tear_drift_dy = 0;
	}

	if ((now_ms - runtime->tear_last_spawn_ms) >= TEAR_DRIFT_RESET_MS) {
		runtime->tear_drift_dy = 0;
		runtime->tear_last_spawn_ms = now_ms;
	} else {
		runtime->tear_drift_dy += 1;
	}
}

/* --- Stage 4: Context bias --- */

static void compute_context_bias(struct face_runtime_state *runtime,
				 const struct pet_state *state)
{
	int16_t openness_bias = 0;
	int16_t brow_bias = 0;
	int16_t mouth_bias = 0;

	if (state->charging) {
		openness_bias += -10;
		brow_bias += 2;
	}

	if (state->battery_low) {
		openness_bias += -15;
		brow_bias += 1;
		mouth_bias += 2;
	}

	if (state->walking_confidence > 50U) {
		openness_bias += 5;
		brow_bias += -1;
	}

	if (state->in_hand && (state->in_hand_confidence > 60U)) {
		openness_bias += 5;
	} else if (!state->in_hand && (state->in_hand_confidence < 15U)) {
		openness_bias += -5;
		brow_bias += 1;
	}

	/* Mood temperament on the face: a content pet carries its mouth a
	 * touch higher, a worn-down one droops. */
	if (state->mood >= 70) {
		mouth_bias += -1;
	} else if (state->mood <= 30) {
		mouth_bias += 1;
		brow_bias += 1;
	}

	/* Alertness opens the eyes a touch; heavy sleepiness drops the lids
	 * beyond what the recipe encodes. */
	if (state->arousal > 60) {
		openness_bias += 4;
	}
	if (state->sleepiness > 75) {
		openness_bias += -8;
	}

	runtime->ctx_openness_bias = openness_bias;
	runtime->ctx_brow_bias_dy = brow_bias;
	runtime->ctx_mouth_bias_dy = mouth_bias;
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
	runtime->ch_eye_openness = FACE_EYE_OPENNESS_DEFAULT;
	runtime->tgt_eye_openness = FACE_EYE_OPENNESS_DEFAULT;
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

	detect_expression_transition(runtime, recipe_id, recipe, now_ms);

	/* Whisker wiggle: start animation when reaction with wiggle just begins */
	if (reaction->has_whisker_wiggle && (reaction_id != runtime->current_reaction_id)) {
		runtime->whisker_anim_start_ms = now_ms;
	}
	if (runtime->whisker_anim_start_ms > 0) {
		int16_t wiggle_dy = evaluate_whisker_wiggle(runtime->whisker_anim_start_ms, now_ms);

		if (wiggle_dy == 0 &&
		    (now_ms - runtime->whisker_anim_start_ms) > g_whisker_wiggle[0].duration_ms) {
			runtime->whisker_anim_start_ms = 0;
		}
		runtime->tgt_left_whisker_dy = wiggle_dy;
		runtime->tgt_right_whisker_dy = wiggle_dy;
	}

	/* Context bias computation (affects blink target) */
	compute_context_bias(runtime, state);

	/* Stage 5: breathing — a slow 1 px bob on mouth and whiskers
	 * whenever no reaction/transition needs those channels. */
	{
		int16_t breath_dy = 0;

		if ((reaction_id == KERFUR_FACE_REACTION_REACTION_NONE) &&
		    !runtime->transition_active && !state->battery_critical) {
			breath_dy = breathing_offset(state, now_ms);
		}
		runtime->ctx_mouth_bias_dy =
			(int16_t)(runtime->ctx_mouth_bias_dy + breath_dy);
		if (runtime->whisker_anim_start_ms == 0) {
			runtime->tgt_left_whisker_dy = breath_dy;
			runtime->tgt_right_whisker_dy = breath_dy;
		}
	}

	/* WAKE_BLINK reaction forces a blink */
	if (reaction_id == KERFUR_FACE_REACTION_REACTION_WAKE_BLINK &&
	    reaction_id != runtime->current_reaction_id) {
		runtime->blink_descending = true;
		runtime->blink_phase_start_ms = now_ms;
		runtime->tgt_eye_openness = 0;
	}

	/* Stage 5: a short settle blink right after big reactions. */
	if ((reaction_id == KERFUR_FACE_REACTION_REACTION_NONE) &&
	    ((previous_reaction_id == KERFUR_FACE_REACTION_REACTION_STARTLE) ||
	     (previous_reaction_id == KERFUR_FACE_REACTION_REACTION_HAPPY_BOUNCE) ||
	     (previous_reaction_id == KERFUR_FACE_REACTION_REACTION_NOTIF_BURST)) &&
	    (face_rand(100U) < 60U)) {
		runtime->blink_next_ms = MIN(runtime->blink_next_ms,
					     now_ms + 250LL + (int64_t)face_rand(300U));
	}

	/* Blink / eye openness update */
	update_blink_state(runtime, recipe, blink_profile_id, state, now_ms, ambient);

	step_channels(runtime, recipe, now_ms);

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
	plan.indicator_count = resolve_indicators(state, recipe, reaction, now_ms,
						  plan.indicator_ids,
						  KERFUR_FACE_MAX_INDICATORS);
	plan.indicator_id = (plan.indicator_count > 0U) ? plan.indicator_ids[0]
							: KERFUR_FACE_INDICATOR_NONE;
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
	/* Derive blink_active from the eye openness channel */
	blink_active = (runtime->ch_eye_openness < 30);

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

	/* Stage 5: wandering idle gaze. When the pupils sit idle (no tilt
	 * gaze, not in hand), the eyes occasionally ease to a random spot and
	 * back. This is now the *only* idle pupil motion (the constant cyclic
	 * ambient drift was removed), so a resting face holds still between
	 * these rare, deliberate glances. */
	update_wander(runtime, state, reaction_id, blink_profile_id, ambient, now_ms);

	const bool idle_pupils =
		(dynamic_reason == FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND) &&
		(reaction_id == KERFUR_FACE_REACTION_REACTION_NONE);

	if (idle_pupils && runtime->wander_active) {
		target_x = runtime->wander_x;
		target_y = runtime->wander_y;
	}

	/* The motion classifier already incorporates in-hand quality and
	 * chaos into look_confidence.  Don't double-penalize by taking
	 * MIN(look_conf, in_hand_conf) — that kills gaze during the
	 * transition edges when in_hand_confidence is moderate but the
	 * classifier is still producing good tilt data.
	 *
	 * Use look_confidence directly, with a gentle walking penalty
	 * (not a hard subtraction that zeroes it out). */
	motion_confidence = state->look_confidence;
	if (state->walking_confidence >= 50U) {
		/* Actively walking: strongly reduce gaze (mostly noise). */
		motion_confidence = (uint8_t)(motion_confidence / 4U);
	} else if (state->walking_confidence > 0U) {
		/* Ambiguous walk/carry: modest reduction. */
		motion_confidence = (uint8_t)((motion_confidence * 3U) / 4U);
	}
	if (state->battery_low) {
		motion_confidence = (uint8_t)MIN(motion_confidence, 70U);
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
			target_x /= 2;
			target_y /= 2;
			motion_confidence = (uint8_t)((motion_confidence * 2U) / 3U);
			motion_speed = MAX(motion_speed, 25U);
			break;
		case FACE_RUNTIME_DYNAMIC_DISABLED_MOTION_NOT_IN_HAND:
			if (idle_pupils && runtime->wander_active) {
				/* Wandering glance: deliberate, unhurried. */
				motion_confidence = MAX(motion_confidence, 60U);
				motion_speed = MAX(motion_speed, 22U);
			} else {
				/* Gaze is decaying back to center — keep enough
				 * confidence/speed that the drift looks deliberate. */
				target_x = (target_x * 2) / 3;
				target_y = (target_y * 2) / 3;
				motion_speed = MAX(motion_speed, 30U);
			}
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
		/* Speed scaling: use confidence to gently modulate, but keep
		 * a high floor so the pupil actually moves at reasonable pace.
		 * Old formula produced speed=11 at conf=20 — far too slow. */
		motion_speed = MAX(20U, (uint8_t)((motion_speed * MAX(motion_confidence, 35U)) / 100U));
	}

	if ((runtime->last_step_ms == 0) || (now_ms < runtime->last_step_ms)) {
		runtime->look_render_x = target_x;
		runtime->look_render_y = target_y;
	} else {
		runtime->look_render_x = smooth_axis(runtime->look_render_x, target_x, motion_speed);
		runtime->look_render_y = smooth_axis(runtime->look_render_y, target_y, motion_speed);
	}

	if (dynamic_allowed || idle_pupils) {
		/* Pupils follow look_render only. The old cyclic ambient drift
		 * (the up-right-down-left idle wobble) was removed; a resting
		 * face stays still and only the occasional wander glance moves
		 * the eyes. */
		if (zone_is_available(left_eye_asset)) {
			resolve_pupil_offsets(left_eye_asset, runtime->look_render_x, runtime->look_render_y,
					      recipe->dynamic_pupil_scale_x,
					      recipe->dynamic_pupil_scale_y,
					      &plan.left_pupil_offset_x, &plan.left_pupil_offset_y);
		}
		if (zone_is_available(right_eye_asset)) {
			resolve_pupil_offsets(right_eye_asset, runtime->look_render_x, runtime->look_render_y,
					      recipe->dynamic_pupil_scale_x,
					      recipe->dynamic_pupil_scale_y,
					      &plan.right_pupil_offset_x, &plan.right_pupil_offset_y);
		}
	}

	resolve_effects(&plan, recipe, reaction);

	/* Stage 3: Pupil swap policy (may defer asset change) */
	resolve_pupil_swap(runtime, recipe, reaction, &plan, now_ms);

	/* Stage 3: Reaction stagger (may revert some assets to base) */
	resolve_reaction_stagger(runtime, reaction, &plan, recipe, now_ms);

	plan.look_target_x = target_x;
	plan.look_target_y = target_y;
	plan.look_render_x = runtime->look_render_x;
	plan.look_render_y = runtime->look_render_y;
	plan.dynamic_pupils_allowed = dynamic_allowed;
	plan.special_eye_mode_active = special_eye_mode_active;
	plan.blink_left_active = blink_active;
	plan.blink_right_active = blink_active;
	plan.dynamic_reason = dynamic_reason;

	/* Copy interpolation channel values to plan */
	plan.left_brow_dy = runtime->ch_left_brow_dy;
	plan.right_brow_dy = runtime->ch_right_brow_dy;
	plan.mouth_dy = runtime->ch_mouth_dy;
	plan.left_whisker_dy = runtime->ch_left_whisker_dy;
	plan.right_whisker_dy = runtime->ch_right_whisker_dy;
	plan.eye_openness = (uint8_t)clamp_s16(runtime->ch_eye_openness, 0, 100);
	plan.left_eye_openness = plan.eye_openness;
	plan.right_eye_openness = plan.eye_openness;
	plan.per_eye_openness = false;
	plan.transition_active = runtime->transition_active;

	/* Stage 3: Microanimations (modify plan after channel copy) */
	apply_notif_ping_wink(runtime, &plan, now_ms);
	apply_annoyed_jitter(runtime, &plan);
	update_tear_drift(runtime, &plan, now_ms);
	plan.tear_drift_dy = runtime->tear_drift_dy;

	/* Stage 3: STARTLE forces eyes wide open */
	if (reaction_id == KERFUR_FACE_REACTION_REACTION_STARTLE) {
		plan.eye_openness = 100U;
		plan.left_eye_openness = 100U;
		plan.right_eye_openness = 100U;
	}

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
		     (previous_overlay_id != runtime->plan.overlay_id) ||
		     runtime->transition_active;

	if (IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG) && should_log) {
		log_plan(&runtime->plan);
	}

	return &runtime->plan;
}

#if IS_ENABLED(CONFIG_KERFUR_FACE_DEBUG)

void face_runtime_debug_force_blink(struct face_runtime_state *runtime)
{
	if (runtime == NULL) {
		return;
	}
	runtime->blink_descending = true;
	runtime->blink_phase_start_ms = runtime->last_step_ms;
	runtime->tgt_eye_openness = 0;
}

void face_runtime_debug_force_openness(struct face_runtime_state *runtime, uint8_t openness)
{
	if (runtime == NULL) {
		return;
	}
	runtime->tgt_eye_openness = (int16_t)MIN(openness, 100U);
	runtime->blink_descending = false;
	runtime->blink_next_ms = runtime->last_step_ms + 10000LL; /* suppress blink for 10s */
}

#endif /* CONFIG_KERFUR_FACE_DEBUG */

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
