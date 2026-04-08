#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ui/ui_renderer.h"

LOG_MODULE_REGISTER(ui_renderer, CONFIG_LOG_DEFAULT_LEVEL);

int ui_renderer_init(void)
{
	LOG_INF("UI disabled by configuration");
	return -ENODEV;
}

void ui_renderer_set_blanked(bool blanked)
{
	ARG_UNUSED(blanked);
}

void ui_renderer_request_debug_dump(void)
{
}

void ui_renderer_render(struct pet_state *state, int64_t now_ms)
{
	ARG_UNUSED(state);
	ARG_UNUSED(now_ms);
}
