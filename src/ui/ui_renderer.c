#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ui/face_runtime.h"
#include "ui/generated/kerfur_face_assets.h"
#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(ui_renderer, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_HAS_CHOSEN(zephyr_display)
#error "No zephyr,display chosen node. Select a display shield."
#endif

#if !defined(CONFIG_LV_USE_IMAGE) || !defined(CONFIG_LV_USE_CANVAS) || \
	!defined(CONFIG_LV_USE_LABEL)
#error "ui_renderer requires CONFIG_LV_USE_IMAGE=y, CONFIG_LV_USE_CANVAS=y, and CONFIG_LV_USE_LABEL=y"
#endif

#define UI_CANVAS_MAX_W 128
#define UI_CANVAS_MAX_H 64

struct ui_runtime {
	const struct device *display;
	lv_obj_t *root;
	lv_obj_t *canvas;
	lv_obj_t *overlay_label;
	bool blanked;
	bool debug_dump_requested;
	int16_t width;
	int16_t height;
	int8_t ambient_shift_x;
	int8_t ambient_shift_y;
	uint8_t shift_phase;
	int64_t last_shift_ms;
	uint8_t contrast;
	struct face_runtime_state face_runtime;
};

static struct ui_runtime g_ui;
static uint8_t g_canvas_buf[LV_CANVAS_BUF_SIZE(UI_CANVAS_MAX_W, UI_CANVAS_MAX_H, 8,
						LV_DRAW_BUF_STRIDE_ALIGN)];

static void draw_bitmap(const struct kerfur_face_bitmap *bitmap, int16_t x, int16_t y,
			bool mirror_x, bool draw_black, lv_opa_t opa)
{
	int16_t row;
	int16_t col;

	if ((bitmap == NULL) || (bitmap->data == NULL)) {
		return;
	}

	for (row = 0; row < bitmap->height; row++) {
		for (col = 0; col < bitmap->width; col++) {
			int16_t src_col = mirror_x ? (bitmap->width - 1 - col) : col;
			uint8_t byte = bitmap->data[(row * bitmap->stride) + (src_col / 8)];
			uint8_t bit = BIT(7 - (src_col % 8));
			int16_t dst_x = x + col;
			int16_t dst_y = y + row;

			if (((byte & bit) != 0U) && (dst_x >= 0) && (dst_y >= 0) &&
			    (dst_x < g_ui.width) && (dst_y < g_ui.height)) {
				lv_canvas_set_px_skip_invalidate(
					g_ui.canvas,
					dst_x,
					dst_y,
					draw_black ? lv_color_black() : lv_color_white(),
					opa
				);
			}
		}
	}
}

static bool should_mirror_on_right(const struct kerfur_face_asset_metadata *asset)
{
	return asset != NULL &&
	       ((asset->flags & (KERFUR_FACE_ASSET_FLAG_MIRRORABLE |
				 KERFUR_FACE_ASSET_FLAG_MIRROR_RIGHT)) != 0U);
}

static void draw_asset(enum kerfur_face_asset_id asset_id, int16_t x, int16_t y, bool right_side,
		       lv_opa_t opa)
{
	const struct kerfur_face_asset_metadata *asset = kerfur_face_asset_get(asset_id);
	const bool draw_black = (asset != NULL) &&
		((asset->flags & KERFUR_FACE_ASSET_FLAG_DRAW_BLACK) != 0U);

	if ((asset == NULL) || (asset->bitmap == NULL)) {
		return;
	}

	draw_bitmap(
		asset->bitmap,
		x,
		y,
		right_side && should_mirror_on_right(asset),
		draw_black,
		opa
	);
}

static struct kerfur_face_point point_with_shift(struct kerfur_face_point point)
{
	point.x += g_ui.ambient_shift_x;
	point.y += g_ui.ambient_shift_y;
	return point;
}

static struct kerfur_face_point remap_eye_slot_position(struct kerfur_face_point base_position,
							enum kerfur_face_asset_id from_asset_id,
							enum kerfur_face_asset_id to_asset_id)
{
	const struct kerfur_face_asset_metadata *from_asset = kerfur_face_asset_get(from_asset_id);
	const struct kerfur_face_asset_metadata *to_asset = kerfur_face_asset_get(to_asset_id);

	if ((from_asset == NULL) || (to_asset == NULL)) {
		return base_position;
	}

	base_position.x += to_asset->anchor_x - from_asset->anchor_x;
	base_position.y += to_asset->anchor_y - from_asset->anchor_y;
	return base_position;
}

static struct kerfur_face_point eye_white_draw_position(const struct face_runtime_plan *plan,
							const struct kerfur_face_recipe *recipe,
							bool left_eye)
{
	return remap_eye_slot_position(left_eye ? plan->layout.left_eye_white :
						      plan->layout.right_eye_white,
				       left_eye ? recipe->left_eye_white : recipe->right_eye_white,
				       left_eye ? plan->left_eye_white : plan->right_eye_white);
}

static struct kerfur_face_point pupil_draw_position(const struct face_runtime_plan *plan,
						    const struct kerfur_face_recipe *recipe,
						    bool left_eye)
{
	struct kerfur_face_point point = eye_white_draw_position(plan, recipe, left_eye);
	const struct kerfur_face_point pupil_offset = left_eye ? plan->layout.left_eyeball :
							       plan->layout.right_eyeball;
	const struct kerfur_face_asset_metadata *base_pupil_asset =
		kerfur_face_asset_get(left_eye ? recipe->left_eyeball : recipe->right_eyeball);
	const struct kerfur_face_asset_metadata *active_pupil_asset =
		kerfur_face_asset_get(left_eye ? plan->left_eyeball : plan->right_eyeball);

	point.x += pupil_offset.x;
	point.y += pupil_offset.y;

	if ((base_pupil_asset != NULL) && (active_pupil_asset != NULL)) {
		point.x += active_pupil_asset->anchor_x - base_pupil_asset->anchor_x;
		point.y += active_pupil_asset->anchor_y - base_pupil_asset->anchor_y;
	}

	point.x += left_eye ? plan->left_pupil_offset_x : plan->right_pupil_offset_x;
	point.y += left_eye ? plan->left_pupil_offset_y : plan->right_pupil_offset_y;
	return point_with_shift(point);
}

static void update_contrast(enum pet_display_state state)
{
	uint8_t target;
	int err;

	target = (state == DISPLAY_AMBIENT) ? 96U : 255U;
	if (g_ui.contrast == target) {
		return;
	}

	err = display_set_contrast(g_ui.display, target);
	if ((err < 0) && (err != -ENOSYS)) {
		LOG_WRN("display_set_contrast failed (%d)", err);
		return;
	}

	g_ui.contrast = target;
}

static void update_pixel_shift(bool ambient, int64_t now_ms)
{
	static const int8_t shift_table[6][2] = {
		{0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0},
	};

	if (!ambient) {
		g_ui.ambient_shift_x = 0;
		g_ui.ambient_shift_y = 0;
		g_ui.shift_phase = 0U;
		g_ui.last_shift_ms = now_ms;
		return;
	}

	if ((now_ms - g_ui.last_shift_ms) < (45 * MSEC_PER_SEC)) {
		return;
	}

	g_ui.shift_phase = (g_ui.shift_phase + 1U) % ARRAY_SIZE(shift_table);
	g_ui.ambient_shift_x = shift_table[g_ui.shift_phase][0];
	g_ui.ambient_shift_y = shift_table[g_ui.shift_phase][1];
	g_ui.last_shift_ms = now_ms;
}

static void update_overlay_label(const struct face_runtime_plan *plan)
{
	const struct kerfur_face_overlay_def *overlay_def = kerfur_face_overlay_get(plan->overlay_id);
	struct kerfur_face_point overlay_point = point_with_shift(plan->layout.overlay);

	if ((overlay_def->render_mode != KERFUR_FACE_OVERLAY_RENDER_BATTERY_PERCENT_TEXT) ||
	    !g_ui.face_runtime.battery_percent_known) {
		lv_obj_add_flag(g_ui.overlay_label, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	lv_label_set_text_fmt(g_ui.overlay_label, "%d%%", g_ui.face_runtime.battery_percent);
	lv_obj_set_pos(g_ui.overlay_label, overlay_point.x, overlay_point.y);
	lv_obj_clear_flag(g_ui.overlay_label, LV_OBJ_FLAG_HIDDEN);
}

static void draw_face_plan(const struct face_runtime_plan *plan, lv_opa_t opa)
{
	const struct kerfur_face_recipe *recipe = kerfur_face_recipe_get(plan->recipe_id);
	struct kerfur_face_point left_eye_base = eye_white_draw_position(plan, recipe, true);
	struct kerfur_face_point right_eye_base = eye_white_draw_position(plan, recipe, false);
	struct kerfur_face_point left_eye_pos = point_with_shift(left_eye_base);
	struct kerfur_face_point right_eye_pos = point_with_shift(right_eye_base);
	struct kerfur_face_point left_blink_pos = remap_eye_slot_position(left_eye_base,
								 plan->left_eye_white,
								 plan->blink_left_eye_white);
	struct kerfur_face_point right_blink_pos = remap_eye_slot_position(right_eye_base,
								  plan->right_eye_white,
								  plan->blink_right_eye_white);
	enum kerfur_face_asset_id left_eye_asset = plan->left_eye_white;
	enum kerfur_face_asset_id right_eye_asset = plan->right_eye_white;
	uint8_t left_openness;
	uint8_t right_openness;
	bool left_eyes_closed;
	bool right_eyes_closed;
	uint8_t index;

	left_blink_pos = point_with_shift(left_blink_pos);
	right_blink_pos = point_with_shift(right_blink_pos);

	/* Eye openness-based asset selection (replaces binary blink) */
	left_openness = plan->per_eye_openness ? plan->left_eye_openness : plan->eye_openness;
	right_openness = plan->per_eye_openness ? plan->right_eye_openness : plan->eye_openness;

	/* Use hysteresis: close at <25, reopen at >=35 */
	left_eyes_closed = plan->blink_left_active &&
			   (plan->blink_left_eye_white != KERFUR_FACE_ASSET_NONE);
	right_eyes_closed = plan->blink_right_active &&
			    (plan->blink_right_eye_white != KERFUR_FACE_ASSET_NONE);

	if (left_openness < 25U) {
		left_eyes_closed = (plan->blink_left_eye_white != KERFUR_FACE_ASSET_NONE);
	} else if (left_openness >= 35U) {
		left_eyes_closed = false;
	}

	if (right_openness < 25U) {
		right_eyes_closed = (plan->blink_right_eye_white != KERFUR_FACE_ASSET_NONE);
	} else if (right_openness >= 35U) {
		right_eyes_closed = false;
	}

	if (left_eyes_closed) {
		left_eye_pos = left_blink_pos;
		left_eye_asset = plan->blink_left_eye_white;
	}

	if (right_eyes_closed) {
		right_eye_pos = right_blink_pos;
		right_eye_asset = plan->blink_right_eye_white;
	}

	draw_asset(left_eye_asset, left_eye_pos.x, left_eye_pos.y, false, opa);
	draw_asset(right_eye_asset, right_eye_pos.x, right_eye_pos.y, true, opa);

	if (!left_eyes_closed && (plan->left_eyeball != KERFUR_FACE_ASSET_NONE)) {
		struct kerfur_face_point left_pupil = pupil_draw_position(plan, recipe, true);

		draw_asset(plan->left_eyeball, left_pupil.x, left_pupil.y, false, opa);
	}

	if (!right_eyes_closed && (plan->right_eyeball != KERFUR_FACE_ASSET_NONE)) {
		struct kerfur_face_point right_pupil = pupil_draw_position(plan, recipe, false);

		draw_asset(plan->right_eyeball, right_pupil.x, right_pupil.y, true, opa);
	}

	/* Brows with smooth offset */
	{
		struct kerfur_face_point lbrow = point_with_shift(plan->layout.left_brow);
		struct kerfur_face_point rbrow = point_with_shift(plan->layout.right_brow);

		lbrow.y += plan->left_brow_dy;
		rbrow.y += plan->right_brow_dy;
		draw_asset(plan->left_brow, lbrow.x, lbrow.y, false, opa);
		draw_asset(plan->right_brow, rbrow.x, rbrow.y, true, opa);
	}

	/* Mouth with smooth offset */
	{
		struct kerfur_face_point mpos = point_with_shift(plan->layout.mouth);

		mpos.y += plan->mouth_dy;
		draw_asset(plan->mouth, mpos.x, mpos.y, false, opa);
	}

	/* Whiskers with smooth offset (includes wiggle) */
	{
		struct kerfur_face_point lwh = point_with_shift(plan->layout.left_whisker);
		struct kerfur_face_point rwh = point_with_shift(plan->layout.right_whisker);

		lwh.y += plan->left_whisker_dy;
		rwh.y += plan->right_whisker_dy;
		draw_asset(plan->whiskers, lwh.x, lwh.y, false, opa);
		draw_asset(plan->whiskers, rwh.x, rwh.y, true, opa);
	}

	for (index = 0U; index < plan->effect_count; index++) {
		struct kerfur_face_point point = point_with_shift(plan->effects[index].position);

		/* Tear drift: add vertical offset to tear effects */
		if ((plan->tear_drift_dy != 0) &&
		    (plan->effects[index].asset_id == KERFUR_FACE_ASSET_EFFECT_TEAR)) {
			point.y += plan->tear_drift_dy;
		}
		draw_asset(plan->effects[index].asset_id, point.x, point.y, false, opa);
	}

	{
		struct kerfur_face_point base = point_with_shift(plan->layout.indicator);
		int16_t cursor_x = base.x;
		uint8_t i;

		for (i = 0U; i < plan->indicator_count; i++) {
			const struct kerfur_face_indicator_def *def =
				kerfur_face_indicator_get(plan->indicator_ids[i]);
			const struct kerfur_face_asset_metadata *meta;

			if ((def == NULL) || (def->asset_id == KERFUR_FACE_ASSET_NONE)) {
				continue;
			}

			meta = kerfur_face_asset_get(def->asset_id);
			if ((meta == NULL) || (meta->bitmap == NULL)) {
				continue;
			}

			draw_asset(def->asset_id, cursor_x, base.y, false, opa);
			cursor_x = (int16_t)(cursor_x + meta->bitmap->width + 1);
		}
	}

	if (plan->overlay_asset != KERFUR_FACE_ASSET_NONE &&
	    kerfur_face_overlay_get(plan->overlay_id)->render_mode ==
		    KERFUR_FACE_OVERLAY_RENDER_BITMAP) {
		struct kerfur_face_point point = point_with_shift(plan->layout.overlay);

		draw_asset(plan->overlay_asset, point.x, point.y, false, opa);
	}
}

int ui_renderer_init(void)
{
	struct display_capabilities caps;
	int err;

	g_ui.display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(g_ui.display)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	display_get_capabilities(g_ui.display, &caps);
	g_ui.width = (int16_t)caps.x_resolution;
	g_ui.height = (int16_t)caps.y_resolution;
	if ((g_ui.width > UI_CANVAS_MAX_W) || (g_ui.height > UI_CANVAS_MAX_H)) {
		LOG_ERR("Display exceeds canvas max (%dx%d > %dx%d)",
			g_ui.width, g_ui.height, UI_CANVAS_MAX_W, UI_CANVAS_MAX_H);
		return -EINVAL;
	}

	g_ui.root = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(g_ui.root);
	lv_obj_set_style_bg_color(g_ui.root, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(g_ui.root, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_size(g_ui.root, g_ui.width, g_ui.height);
	lv_obj_set_pos(g_ui.root, 0, 0);
	lv_obj_clear_flag(g_ui.root, LV_OBJ_FLAG_SCROLLABLE);

	g_ui.canvas = lv_canvas_create(g_ui.root);
	lv_canvas_set_buffer(g_ui.canvas, g_canvas_buf, g_ui.width, g_ui.height, LV_COLOR_FORMAT_L8);
	lv_obj_set_pos(g_ui.canvas, 0, 0);
	lv_obj_set_size(g_ui.canvas, g_ui.width, g_ui.height);
	lv_canvas_fill_bg(g_ui.canvas, lv_color_black(), LV_OPA_COVER);

	g_ui.overlay_label = lv_label_create(g_ui.root);
	lv_obj_set_style_text_color(g_ui.overlay_label, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(g_ui.overlay_label, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_add_flag(g_ui.overlay_label, LV_OBJ_FLAG_HIDDEN);

	err = display_blanking_off(g_ui.display);
	if ((err < 0) && (err != -ENOSYS)) {
		LOG_WRN("display_blanking_off failed (%d)", err);
	}

	g_ui.blanked = false;
	g_ui.debug_dump_requested = false;
	g_ui.ambient_shift_x = 0;
	g_ui.ambient_shift_y = 0;
	g_ui.shift_phase = 0U;
	g_ui.last_shift_ms = k_uptime_get();
	g_ui.contrast = 255U;
	face_runtime_init(&g_ui.face_runtime);
	lv_timer_handler();
	return 0;
}

void ui_renderer_set_blanked(bool blanked)
{
	int err;

	if ((g_ui.display == NULL) || (g_ui.root == NULL) || (blanked == g_ui.blanked)) {
		return;
	}

	g_ui.blanked = blanked;
	if (blanked) {
		lv_obj_add_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);
		err = display_blanking_on(g_ui.display);
		if ((err < 0) && (err != -ENOSYS)) {
			LOG_WRN("display_blanking_on failed (%d)", err);
		}
	} else {
		lv_obj_clear_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);
		err = display_blanking_off(g_ui.display);
		if ((err < 0) && (err != -ENOSYS)) {
			LOG_WRN("display_blanking_off failed (%d)", err);
		}
	}
}

void ui_renderer_request_debug_dump(void)
{
	g_ui.debug_dump_requested = true;
}

void ui_renderer_render(struct pet_state *state, int64_t now_ms)
{
	const bool ambient = (state->current_display_state == DISPLAY_AMBIENT);
	const struct face_runtime_plan *plan;
	lv_opa_t opa = ambient ? LV_OPA_50 : LV_OPA_COVER;

	if ((g_ui.canvas == NULL) || g_ui.blanked || (state->current_display_state == DISPLAY_OFF)) {
		return;
	}

	update_contrast(state->current_display_state);
	update_pixel_shift(ambient, now_ms);
	plan = face_runtime_step(&g_ui.face_runtime, state, now_ms, ambient,
				 g_ui.debug_dump_requested);
	g_ui.debug_dump_requested = false;

	lv_canvas_fill_bg(g_ui.canvas, lv_color_black(), LV_OPA_COVER);
	lv_obj_add_flag(g_ui.overlay_label, LV_OBJ_FLAG_HIDDEN);

	if (plan != NULL) {
		draw_face_plan(plan, opa);
		update_overlay_label(plan);
	}

	lv_obj_invalidate(g_ui.canvas);
	lv_obj_invalidate(g_ui.overlay_label);
	lv_timer_handler();
}
