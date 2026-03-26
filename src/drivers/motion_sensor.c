#include <string.h>

#include <zephyr/kernel.h>

#include "drivers/motion_sensor.h"

static struct motion_sensor_capabilities g_caps;
static enum motion_sensor_mode g_mode = MOTION_SENSOR_MODE_DISABLED;
static motion_sensor_event_handler_t g_event_handler;
static void *g_event_user_data;

int motion_sensor_init(void)
{
	memset(&g_caps, 0, sizeof(g_caps));
	g_mode = MOTION_SENSOR_MODE_DISABLED;
	return -ENODEV;
}

bool motion_sensor_is_ready(void)
{
	return g_caps.backend_ready;
}

enum motion_sensor_mode motion_sensor_get_mode(void)
{
	return g_mode;
}

const struct motion_sensor_capabilities *motion_sensor_get_capabilities(void)
{
	return &g_caps;
}

int motion_sensor_set_event_handler(motion_sensor_event_handler_t handler, void *user_data)
{
	g_event_handler = handler;
	g_event_user_data = user_data;
	return 0;
}

int motion_sensor_set_mode(enum motion_sensor_mode mode)
{
	g_mode = mode;
	return 0;
}

int motion_sensor_fetch_sample(struct motion_sensor_sample *out_sample)
{
	if (out_sample == NULL) {
		return -EINVAL;
	}

	memset(out_sample, 0, sizeof(*out_sample));
	return -ENODEV;
}

int motion_sensor_read_hw_step_counter(uint16_t *out_counter)
{
	if (out_counter == NULL) {
		return -EINVAL;
	}

	*out_counter = 0U;
	return -ENOTSUP;
}
