#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(ui_renderer, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_HAS_CHOSEN(zephyr_display)
#error "No zephyr,display chosen node. Select a display shield."
#endif

struct face_nodes {
	lv_obj_t *root;
	lv_obj_t *eye_l;
	lv_obj_t *eye_r;
	lv_obj_t *mouth;
	lv_obj_t *mouth_aux;
	lv_obj_t *brow_l;
	lv_obj_t *brow_r;
	lv_obj_t *icon_ble;
	lv_obj_t *icon_bat_body;
	lv_obj_t *icon_bat_tip;
	lv_obj_t *icon_notif;
};

static const struct device *g_display;
static struct face_nodes g_face;
static bool g_blanked;
static int16_t g_width;
static int16_t g_height;
static uint32_t g_frame_id;
static uint8_t g_shift_x;
static uint8_t g_shift_y;

static void ui_rect_style(lv_obj_t *obj, lv_color_t color)
{
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
}

static lv_obj_t *ui_make_rect(lv_obj_t *parent, lv_color_t color)
{
	lv_obj_t *obj = lv_obj_create(parent);

	ui_rect_style(obj, color);
	return obj;
}

static void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
	if (hidden) {
		lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
	}
}

static void ui_create_scene(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_color_t white = lv_color_hex(0xffffff);
	lv_color_t black = lv_color_hex(0x000000);

	g_face.root = lv_obj_create(screen);
	ui_rect_style(g_face.root, black);
	lv_obj_set_size(g_face.root, g_width, g_height);
	lv_obj_set_pos(g_face.root, 0, 0);
	lv_obj_clear_flag(g_face.root, LV_OBJ_FLAG_SCROLLABLE);

	g_face.eye_l = ui_make_rect(g_face.root, white);
	g_face.eye_r = ui_make_rect(g_face.root, white);
	g_face.mouth = ui_make_rect(g_face.root, white);
	g_face.mouth_aux = ui_make_rect(g_face.root, white);
	g_face.brow_l = ui_make_rect(g_face.root, white);
	g_face.brow_r = ui_make_rect(g_face.root, white);
	g_face.icon_ble = ui_make_rect(g_face.root, white);
	g_face.icon_bat_body = ui_make_rect(g_face.root, white);
	g_face.icon_bat_tip = ui_make_rect(g_face.root, white);
	g_face.icon_notif = ui_make_rect(g_face.root, white);
}

static void ui_update_icons(const struct pet_state *state, int64_t now_ms)
{
	bool notif_recent = (state->last_phone_notification_timestamp_ms > 0) &&
			    ((now_ms - state->last_phone_notification_timestamp_ms) < (8 * MSEC_PER_SEC));
	bool notif_pulse = ((now_ms / 250) % 2) == 0;

	ui_set_hidden(g_face.icon_ble, !state->ble_connected);
	ui_set_hidden(g_face.icon_bat_body, !state->battery_low);
	ui_set_hidden(g_face.icon_bat_tip, !state->battery_low);
	ui_set_hidden(g_face.icon_notif, !(notif_recent && notif_pulse));

	if (state->ble_connected) {
		lv_obj_set_pos(g_face.icon_ble, g_width - 8, 2);
		lv_obj_set_size(g_face.icon_ble, 4, 4);
	}

	if (state->battery_low) {
		lv_obj_set_pos(g_face.icon_bat_body, 2, 2);
		lv_obj_set_size(g_face.icon_bat_body, 6, 4);
		lv_obj_set_pos(g_face.icon_bat_tip, 8, 3);
		lv_obj_set_size(g_face.icon_bat_tip, 1, 2);
	}

	if (notif_recent && notif_pulse) {
		lv_obj_set_pos(g_face.icon_notif, (g_width / 2) - 2, 2);
		lv_obj_set_size(g_face.icon_notif, 4, 4);
	}
}

int ui_renderer_init(void)
{
	struct display_capabilities caps;
	int err;

	g_display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(g_display)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	display_get_capabilities(g_display, &caps);
	g_width = (int16_t)caps.x_resolution;
	g_height = (int16_t)caps.y_resolution;

	ui_create_scene();
	lv_timer_handler();

	err = display_blanking_off(g_display);
	if (err < 0 && err != -ENOSYS) {
		LOG_WRN("display_blanking_off failed (%d)", err);
	}

	g_blanked = false;
	g_frame_id = 0U;
	g_shift_x = 0U;
	g_shift_y = 0U;

	LOG_INF("LVGL renderer ready (%dx%d)", g_width, g_height);
	return 0;
}

void ui_renderer_set_blanked(bool blanked)
{
	int err;

	if ((g_display == NULL) || (g_face.root == NULL) || (blanked == g_blanked)) {
		return;
	}

	g_blanked = blanked;
	ui_set_hidden(g_face.root, blanked);

	if (blanked) {
		err = display_blanking_on(g_display);
		if (err < 0 && err != -ENOSYS) {
			LOG_WRN("display_blanking_on failed (%d)", err);
		}
	} else {
		err = display_blanking_off(g_display);
		if (err < 0 && err != -ENOSYS) {
			LOG_WRN("display_blanking_off failed (%d)", err);
		}
		lv_timer_handler();
	}
}

void ui_renderer_render(const struct pet_state *state, int64_t now_ms)
{
	int16_t center_x;
	int16_t center_y;
	int16_t left_x;
	int16_t right_x;
	int16_t eye_y;
	int16_t eye_w = 12;
	int16_t eye_h;
	int16_t mouth_x;
	int16_t mouth_y;
	int16_t mouth_w = 20;
	int16_t mouth_h = 2;
	bool blink;
	bool phase;

	if ((g_face.root == NULL) || g_blanked) {
		return;
	}

	g_frame_id++;
	if ((g_frame_id % 70U) == 0U) {
		/* Anti-burn-in baseline: shift the face slowly by a few pixels. */
		g_shift_x = (g_shift_x + 1U) % 3U;
		g_shift_y = (g_shift_y + 1U) % 2U;
	}
	lv_obj_set_pos(g_face.root, (int16_t)g_shift_x, (int16_t)g_shift_y);

	blink = (((now_ms / 120) % 28) == 0) || (((now_ms / 120) % 28) == 1);
	phase = ((now_ms / 350) % 2) == 0;

	center_x = g_width / 2;
	center_y = g_height / 2;
	eye_y = center_y - 10;
	left_x = center_x - 26;
	right_x = center_x + 14;
	eye_h = blink ? 2 : 8;

	ui_set_hidden(g_face.brow_l, true);
	ui_set_hidden(g_face.brow_r, true);
	ui_set_hidden(g_face.mouth_aux, true);

	switch (state->expression) {
	case PET_EXPR_SLEEPY:
		eye_h = 3;
		mouth_w = 24;
		mouth_h = 1;
		break;
	case PET_EXPR_ASLEEP:
		eye_h = 1;
		mouth_w = 12;
		mouth_h = 1;
		break;
	case PET_EXPR_CURIOUS:
		mouth_w = 4;
		mouth_h = 4;
		break;
	case PET_EXPR_NEEDY:
		mouth_w = 24;
		mouth_h = 2;
		ui_set_hidden(g_face.mouth_aux, false);
		break;
	case PET_EXPR_STRESSED:
		mouth_w = 24;
		mouth_h = 2;
		ui_set_hidden(g_face.brow_l, false);
		ui_set_hidden(g_face.brow_r, false);
		break;
	case PET_EXPR_HAPPY:
		mouth_w = 22;
		mouth_h = 3;
		break;
	case PET_EXPR_IDLE:
	default:
		mouth_w = 20;
		mouth_h = 2;
		break;
	}

	lv_obj_set_pos(g_face.eye_l, left_x, eye_y);
	lv_obj_set_size(g_face.eye_l, eye_w, eye_h);

	if (state->expression == PET_EXPR_CURIOUS) {
		lv_obj_set_pos(g_face.eye_r, right_x, eye_y - 1);
		lv_obj_set_size(g_face.eye_r, eye_w, eye_h + 3);
	} else {
		lv_obj_set_pos(g_face.eye_r, right_x, eye_y);
		lv_obj_set_size(g_face.eye_r, eye_w, eye_h);
	}

	if (state->expression == PET_EXPR_ASLEEP) {
		mouth_x = center_x - (mouth_w / 2);
		mouth_y = center_y + 11;
	} else if (state->expression == PET_EXPR_CURIOUS) {
		mouth_x = center_x - 2;
		mouth_y = center_y + (phase ? 8 : 9);
	} else {
		mouth_x = center_x - (mouth_w / 2);
		mouth_y = center_y + (phase ? 10 : 11);
	}

	lv_obj_set_pos(g_face.mouth, mouth_x, mouth_y);
	lv_obj_set_size(g_face.mouth, mouth_w, mouth_h);

	if (state->expression == PET_EXPR_NEEDY) {
		lv_obj_set_pos(g_face.mouth_aux, center_x - 2, mouth_y - 2);
		lv_obj_set_size(g_face.mouth_aux, 4, 1);
	}

	if (state->expression == PET_EXPR_STRESSED) {
		lv_obj_set_pos(g_face.brow_l, left_x - 2, eye_y - 2);
		lv_obj_set_size(g_face.brow_l, 6, 1);
		lv_obj_set_pos(g_face.brow_r, right_x + eye_w - 4, eye_y - 2);
		lv_obj_set_size(g_face.brow_r, 6, 1);
	}

	ui_update_icons(state, now_ms);
	lv_timer_handler();
}
