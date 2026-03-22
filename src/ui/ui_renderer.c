#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "behavior/micro_reaction.h"
#include "ui/kerfur_faces.h"
#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(ui_renderer, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_HAS_CHOSEN(zephyr_display)
#error "No zephyr,display chosen node. Select a display shield."
#endif

#define UI_CANVAS_MAX_W 128
#define UI_CANVAS_MAX_H 64

#define BASE_LEFT_EYE_X 17
#define BASE_RIGHT_EYE_X 74
#define BASE_EYE_Y 16
#define BASE_MOUTH_X 54
#define BASE_MOUTH_Y 38
#define BASE_WHISKER_LEFT_X 0
#define BASE_WHISKER_RIGHT_X 119
#define BASE_WHISKER_Y 46
#define BASE_BROW_LEFT_X 46
#define BASE_BROW_RIGHT_X 72
#define BASE_BROW_Y 8
#define BASE_BLINK_LEFT_X 20
#define BASE_BLINK_RIGHT_X 77
#define BASE_BLINK_Y 30

struct ui_runtime {
	const struct device *display;
	lv_obj_t *root;
	lv_obj_t *canvas;
	bool blanked;
	int16_t width;
	int16_t height;
	int8_t ambient_shift_x;
	int8_t ambient_shift_y;
	uint8_t shift_phase;
	int64_t last_shift_ms;
	uint8_t contrast;
};

static struct ui_runtime g_ui;
static uint8_t g_canvas_buf[LV_CANVAS_BUF_SIZE(UI_CANVAS_MAX_W, UI_CANVAS_MAX_H, 8,
						LV_DRAW_BUF_STRIDE_ALIGN)];

static void draw_pixel(int16_t x, int16_t y, lv_opa_t opa)
{
	if ((x < 0) || (y < 0) || (x >= g_ui.width) || (y >= g_ui.height)) {
		return;
	}

	lv_canvas_set_px_skip_invalidate(g_ui.canvas, x, y, lv_color_white(), opa);
}

static void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, lv_opa_t opa)
{
	int16_t px;
	int16_t py;

	for (py = 0; py < h; py++) {
		for (px = 0; px < w; px++) {
			draw_pixel(x + px, y + py, opa);
		}
	}
}

static void draw_bitmap(const struct kerfur_bitmap *bmp, int16_t x, int16_t y, lv_opa_t opa)
{
	uint8_t bytes_per_row;
	int16_t row;
	int16_t col;

	if ((bmp == NULL) || (bmp->data == NULL)) {
		return;
	}

	bytes_per_row = (uint8_t)((bmp->width + 7U) / 8U);

	for (row = 0; row < bmp->height; row++) {
		for (col = 0; col < bmp->width; col++) {
			const uint8_t byte = bmp->data[(row * bytes_per_row) + (col / 8)];
			/*
			 * Adafruit GFX-style bitmap data is MSB-first in each byte.
			 * Using MSB->left fixes mirrored/scrambled columns.
			 */
			const uint8_t bit = BIT(7 - (col % 8));

			if ((byte & bit) != 0U) {
				draw_pixel(x + col, y + row, opa);
			}
		}
	}
}

static void update_contrast(enum pet_display_state state)
{
	uint8_t target;
	int err;

	if (state == DISPLAY_AMBIENT) {
		target = 96U;
	} else {
		target = 255U;
	}

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

static void resolve_face_pose(const struct pet_state *state, enum kerfur_face_visual visual,
			      bool ambient, int64_t now_ms, struct kerfur_face_pose *pose)
{
	const struct kerfur_face_animation *idle_animation;
	const struct kerfur_face_animation *reaction_animation;

	kerfur_face_pose_init(visual, pose);

	idle_animation = kerfur_face_idle_animation_get(visual, ambient);
	(void)kerfur_face_pose_apply_animation(pose, idle_animation, now_ms);

	reaction_animation = kerfur_face_reaction_animation_get(visual, state->current_reaction);
	if (reaction_animation != NULL) {
		(void)kerfur_face_pose_apply_animation(pose, reaction_animation,
						      now_ms - state->last_reaction_timestamp_ms);
	}
}

static bool should_blink(const struct pet_state *state, enum kerfur_face_visual visual,
			 int64_t now_ms, bool ambient,
			 const struct kerfur_face_pose *pose)
{
	const int64_t blink_period_ms = ambient ? 8000 : 4200;
	const bool reaction_anim_active =
		kerfur_face_reaction_animation_get(visual, state->current_reaction) != NULL;
	const int64_t blink_duration_ms =
		((pose->flags & KERFUR_FACE_FLAG_SLEEPY) != 0U) ? 400 : 220;
	const bool periodic = ((now_ms + (state->arousal * 31)) % blink_period_ms) < blink_duration_ms;

	if ((pose != NULL) && (pose->eye_mode == KERFUR_FACE_EYE_BLINK)) {
		return true;
	}

	if (reaction_anim_active) {
		return false;
	}

	return periodic;
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

	g_ui.shift_phase = (g_ui.shift_phase + 1U) % 6U;
	g_ui.ambient_shift_x = shift_table[g_ui.shift_phase][0];
	g_ui.ambient_shift_y = shift_table[g_ui.shift_phase][1];
	g_ui.last_shift_ms = now_ms;
}

static void draw_reaction_overlay(const struct pet_state *state, int64_t now_ms, lv_opa_t opa)
{
	switch (state->current_reaction) {
	case REACTION_STARTLE:
		draw_rect((g_ui.width / 2) - 1, 2, 3, 8, opa);
		draw_rect((g_ui.width / 2) - 1, 12, 3, 3, opa);
		break;
	case REACTION_NOTIF_PING:
		draw_rect((g_ui.width / 2) - 2, 3, 4, 4, opa);
		break;
	case REACTION_NOTIF_BURST:
		draw_rect((g_ui.width / 2) - 4, 2, 8, 4, opa);
		draw_rect((g_ui.width / 2) - 2, 7, 4, 3, opa);
		break;
	case REACTION_CHARGE_PULSE:
		if (((now_ms / 250) % 2) == 0) {
			draw_rect(4, 3, 7, 4, opa);
			draw_rect(11, 4, 2, 2, opa);
		}
		break;
	case REACTION_LOW_BATT_SAG:
		draw_rect(4, 3, 7, 4, opa / 2);
		draw_rect(11, 4, 2, 2, opa / 2);
		break;
	case REACTION_CONNECT_SPARK:
		draw_rect(g_ui.width - 10, 2, 2, 6, opa);
		draw_rect(g_ui.width - 7, 4, 2, 4, opa);
		break;
	case REACTION_DISCONNECT_SCAN:
		draw_rect(g_ui.width - 11, 3, 6, 2, opa);
		draw_rect(g_ui.width - 11, 7, 4, 2, opa);
		break;
	case REACTION_SLEEP_FADE:
		draw_rect(0, 0, g_ui.width, g_ui.height, LV_OPA_30);
		break;
	default:
		break;
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

	err = display_blanking_off(g_ui.display);
	if ((err < 0) && (err != -ENOSYS)) {
		LOG_WRN("display_blanking_off failed (%d)", err);
	}

	g_ui.blanked = false;
	g_ui.ambient_shift_x = 0;
	g_ui.ambient_shift_y = 0;
	g_ui.shift_phase = 0U;
	g_ui.last_shift_ms = k_uptime_get();
	g_ui.contrast = 255U;
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

void ui_renderer_render(const struct pet_state *state, int64_t now_ms)
{
	const enum kerfur_face_visual visual = kerfur_face_visual_for_expression(state->current_expression);
	const struct kerfur_face_asset_pack *assets = kerfur_face_assets_get(visual);
	const bool ambient = (state->current_display_state == DISPLAY_AMBIENT);
	struct kerfur_face_pose pose;
	bool blink;
	lv_opa_t opa = ambient ? LV_OPA_50 : LV_OPA_COVER;
	int16_t left_eye_x;
	int16_t right_eye_x;
	int16_t eye_y;
	int16_t mouth_x;
	int16_t mouth_y;
	int16_t brow_y;

	if ((g_ui.canvas == NULL) || g_ui.blanked || (state->current_display_state == DISPLAY_OFF)) {
		return;
	}

	resolve_face_pose(state, visual, ambient, now_ms, &pose);
	blink = should_blink(state, visual, now_ms, ambient, &pose);

	update_contrast(state->current_display_state);
	update_pixel_shift(ambient, now_ms);
	lv_canvas_fill_bg(g_ui.canvas, lv_color_black(), LV_OPA_COVER);

	left_eye_x = assets->eye_left_x + pose.eye_dx + g_ui.ambient_shift_x;
	right_eye_x = assets->eye_right_x + pose.eye_dx + g_ui.ambient_shift_x;
	eye_y = assets->eye_y + pose.eye_dy + g_ui.ambient_shift_y;
	mouth_x = assets->mouth_x + pose.mouth_dx + g_ui.ambient_shift_x;
	mouth_y = assets->mouth_y + pose.mouth_dy + g_ui.ambient_shift_y;
	brow_y = assets->brow_y + pose.brow_dy + g_ui.ambient_shift_y;

	if ((pose.flags & KERFUR_FACE_FLAG_SHOW_WHISKERS) != 0U) {
		draw_bitmap(assets->whisker_left,
			    assets->whisker_left_x + g_ui.ambient_shift_x,
			    assets->whisker_y + pose.whisker_dy + g_ui.ambient_shift_y, opa);
		draw_bitmap(assets->whisker_right,
			    assets->whisker_right_x + g_ui.ambient_shift_x,
			    assets->whisker_y + pose.whisker_dy + g_ui.ambient_shift_y, opa);
	}

	if (blink) {
		draw_bitmap(assets->eye_blink,
			    assets->blink_left_x + pose.eye_dx + g_ui.ambient_shift_x,
			    assets->blink_y + pose.eye_dy + g_ui.ambient_shift_y, opa);
		draw_bitmap(assets->eye_blink,
			    assets->blink_right_x + pose.eye_dx + g_ui.ambient_shift_x,
			    assets->blink_y + pose.eye_dy + g_ui.ambient_shift_y, opa);
	} else {
		draw_bitmap(assets->eye_open_left, left_eye_x, eye_y, opa);
		draw_bitmap(assets->eye_open_right, right_eye_x, eye_y, opa);
	}

	draw_bitmap(assets->mouth, mouth_x, mouth_y, opa);

	if (((pose.flags & KERFUR_FACE_FLAG_SHOW_MOUTH_OPEN) != 0U) && (assets->mouth_open != NULL)) {
		draw_bitmap(assets->mouth_open,
			    assets->mouth_open_x + g_ui.ambient_shift_x,
			    assets->mouth_open_y + pose.mouth_open_dy + g_ui.ambient_shift_y, opa);
	}

	if ((pose.flags & KERFUR_FACE_FLAG_SHOW_BROWS) != 0U) {
		draw_bitmap(assets->brow_left, assets->brow_left_x + g_ui.ambient_shift_x,
			    brow_y, opa);
		draw_bitmap(assets->brow_right, assets->brow_right_x + g_ui.ambient_shift_x,
			    brow_y, opa);
	}

	if (state->ble_connected) {
		draw_rect(g_ui.width - 6, 2, 3, 4, opa);
	}

	if (state->battery_low) {
		draw_rect(2, 2, 6, 4, opa / 2);
		draw_rect(8, 3, 1, 2, opa / 2);
	}

	draw_reaction_overlay(state, now_ms, opa);
	lv_obj_invalidate(g_ui.canvas);
	lv_timer_handler();
}
