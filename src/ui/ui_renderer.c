#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(ui_renderer, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_HAS_CHOSEN(zephyr_display)
#error "No zephyr,display chosen node. Add it in board overlay."
#endif

#define SCREEN_WIDTH_MAX 128
#define SCREEN_HEIGHT_MAX 64
#define FB_SIZE_MAX ((SCREEN_WIDTH_MAX * SCREEN_HEIGHT_MAX) / 8)

static const struct device *g_display;
static uint8_t g_fb[FB_SIZE_MAX];
static uint16_t g_width = SCREEN_WIDTH_MAX;
static uint16_t g_height = SCREEN_HEIGHT_MAX;
static bool g_blanked;
static uint8_t g_shift_x;
static uint8_t g_shift_y;
static uint32_t g_frame_id;

static int iabs(int value)
{
	return (value < 0) ? -value : value;
}

static void fb_clear(void)
{
	(void)memset(g_fb, 0, sizeof(g_fb));
}

static void fb_set_pixel(int x, int y, bool on)
{
	size_t index;
	uint8_t bit;

	x += g_shift_x;
	y += g_shift_y;

	if ((x < 0) || (y < 0) || (x >= g_width) || (y >= g_height)) {
		return;
	}

	index = (size_t)x + ((size_t)y / 8U) * g_width;
	bit = BIT(y & 0x7);

	if (on) {
		g_fb[index] |= bit;
	} else {
		g_fb[index] &= ~bit;
	}
}

static void fb_fill_rect(int x, int y, int w, int h)
{
	int ix;
	int iy;

	for (iy = y; iy < (y + h); iy++) {
		for (ix = x; ix < (x + w); ix++) {
			fb_set_pixel(ix, iy, true);
		}
	}
}

static void fb_draw_line(int x0, int y0, int x1, int y1)
{
	int dx = iabs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -iabs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;

	while (true) {
		fb_set_pixel(x0, y0, true);
		if ((x0 == x1) && (y0 == y1)) {
			break;
		}

		if ((2 * err) >= dy) {
			err += dy;
			x0 += sx;
		}
		if ((2 * err) <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void draw_eyes(enum pet_expression expression, bool blink)
{
	int eye_y = (int)(g_height / 2U) - 10;
	int left_x = (int)(g_width / 2U) - 26;
	int right_x = (int)(g_width / 2U) + 14;
	int eye_w = 12;
	int eye_h = blink ? 2 : 8;

	if (expression == PET_EXPR_SLEEPY) {
		eye_h = 3;
	}

	if (expression == PET_EXPR_ASLEEP) {
		fb_draw_line(left_x, eye_y + 2, left_x + eye_w, eye_y + 2);
		fb_draw_line(right_x, eye_y + 2, right_x + eye_w, eye_y + 2);
		return;
	}

	fb_fill_rect(left_x, eye_y, eye_w, eye_h);
	fb_fill_rect(right_x, eye_y, eye_w, eye_h);

	if ((expression == PET_EXPR_CURIOUS) && !blink) {
		fb_fill_rect(right_x + 3, eye_y - 2, eye_w - 6, 3);
	}

	if ((expression == PET_EXPR_STRESSED) && !blink) {
		fb_set_pixel(left_x - 2, eye_y - 2, true);
		fb_set_pixel(left_x - 1, eye_y - 1, true);
		fb_set_pixel(right_x + eye_w + 1, eye_y - 2, true);
		fb_set_pixel(right_x + eye_w, eye_y - 1, true);
	}
}

static void draw_mouth(enum pet_expression expression, int64_t now_ms)
{
	int base_x = (int)(g_width / 2U) - 12;
	int base_y = (int)(g_height / 2U) + 10;
	bool phase = ((now_ms / 350) % 2) == 0;

	switch (expression) {
	case PET_EXPR_HAPPY:
		fb_draw_line(base_x, base_y, base_x + 10, base_y + 4);
		fb_draw_line(base_x + 10, base_y + 4, base_x + 24, base_y);
		break;
	case PET_EXPR_SLEEPY:
		fb_draw_line(base_x, base_y + 2, base_x + 24, base_y + 2);
		break;
	case PET_EXPR_CURIOUS:
		fb_fill_rect(base_x + 10, base_y + (phase ? 0 : 1), 4, 4);
		break;
	case PET_EXPR_NEEDY:
		fb_draw_line(base_x, base_y + 3, base_x + 24, base_y + 3);
		fb_fill_rect(base_x + 10, base_y + 1, 4, 2);
		break;
	case PET_EXPR_STRESSED:
		fb_draw_line(base_x, base_y + 2, base_x + 6, base_y);
		fb_draw_line(base_x + 6, base_y, base_x + 12, base_y + 4);
		fb_draw_line(base_x + 12, base_y + 4, base_x + 18, base_y);
		fb_draw_line(base_x + 18, base_y, base_x + 24, base_y + 2);
		break;
	case PET_EXPR_ASLEEP:
		fb_draw_line(base_x + 6, base_y + 2, base_x + 18, base_y + 2);
		break;
	case PET_EXPR_IDLE:
	default:
		fb_draw_line(base_x, base_y + (phase ? 1 : 2), base_x + 24, base_y + (phase ? 1 : 2));
		break;
	}
}

static void draw_status_icons(const struct pet_state *state)
{
	if (state->ble_connected) {
		fb_fill_rect(g_width - 8, 2, 4, 4);
	}
	if (state->battery_low) {
		fb_draw_line(3, 3, 8, 3);
		fb_draw_line(3, 3, 3, 6);
		fb_draw_line(8, 3, 8, 6);
		fb_draw_line(3, 6, 8, 6);
		fb_set_pixel(9, 4, true);
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
	g_width = MIN((uint16_t)caps.x_resolution, (uint16_t)SCREEN_WIDTH_MAX);
	g_height = MIN((uint16_t)caps.y_resolution, (uint16_t)SCREEN_HEIGHT_MAX);

	err = display_set_pixel_format(g_display, PIXEL_FORMAT_MONO10);
	if (err) {
		LOG_WRN("Could not set MONO10 pixel format (%d)", err);
	}

	err = display_blanking_off(g_display);
	if (err) {
		LOG_WRN("display_blanking_off failed (%d)", err);
	}

	g_blanked = false;
	g_shift_x = 0;
	g_shift_y = 0;
	g_frame_id = 0;
	fb_clear();

	LOG_INF("UI ready (%ux%u)", g_width, g_height);
	return 0;
}

void ui_renderer_set_blanked(bool blanked)
{
	if (g_display == NULL) {
		return;
	}

	if (blanked == g_blanked) {
		return;
	}

	g_blanked = blanked;
	if (blanked) {
		(void)display_blanking_on(g_display);
	} else {
		(void)display_blanking_off(g_display);
	}
}

void ui_renderer_render(const struct pet_state *state, int64_t now_ms)
{
	struct display_buffer_descriptor desc;
	bool blink;
	int err;

	if ((g_display == NULL) || g_blanked) {
		return;
	}

	g_frame_id++;
	if ((g_frame_id % 70U) == 0U) {
		/* Slow pixel shifting as anti-burn-in foundation. */
		g_shift_x = (g_shift_x + 1U) % 3U;
		g_shift_y = (g_shift_y + 1U) % 2U;
	}

	blink = (((now_ms / 120) % 28) == 0) || (((now_ms / 120) % 28) == 1);

	fb_clear();
	draw_eyes(state->expression, blink);
	draw_mouth(state->expression, now_ms);
	draw_status_icons(state);

	desc.buf_size = (g_width * g_height) / 8U;
	desc.width = g_width;
	desc.height = g_height;
	desc.pitch = g_width;
	desc.frame_incomplete = false;

	err = display_write(g_display, 0, 0, &desc, g_fb);
	if (err) {
		LOG_WRN("display_write failed (%d)", err);
	}
}
