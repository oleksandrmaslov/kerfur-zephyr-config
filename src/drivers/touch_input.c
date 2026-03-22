#include <errno.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "drivers/touch_input.h"

LOG_MODULE_REGISTER(touch_input, CONFIG_LOG_DEFAULT_LEVEL);

#define TOUCH_DEBOUNCE_MS 80

#if DT_NODE_HAS_STATUS(DT_ALIAS(touch0), okay)
static const struct gpio_dt_spec g_touch0 = GPIO_DT_SPEC_GET(DT_ALIAS(touch0), gpios);
static struct gpio_callback g_touch0_cb;
static int64_t g_touch0_last_event_ms;
#endif

static void publish_with_best_effort(enum app_event_type type)
{
	if (app_event_publish(type, 0) != 0) {
		LOG_WRN("Dropped event %s (queue full)", app_event_type_str(type));
	}
}

#if DT_NODE_HAS_STATUS(DT_ALIAS(touch0), okay)
static void touch0_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	int64_t now_ms;

	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	now_ms = k_uptime_get();
	if ((now_ms - g_touch0_last_event_ms) < TOUCH_DEBOUNCE_MS) {
		return;
	}

	g_touch0_last_event_ms = now_ms;
	publish_with_best_effort(APP_EVENT_USER_PET_SOFT);

	LOG_INF("touch0 active -> USER_PET_SOFT");
}
#endif

int touch_input_init(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(touch0), okay)
	int err;

	if (!gpio_is_ready_dt(&g_touch0)) {
		LOG_WRN("touch0 GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&g_touch0, GPIO_INPUT);
	if (err) {
		LOG_ERR("touch0 configure failed (%d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&g_touch0, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("touch0 interrupt config failed (%d)", err);
		return err;
	}

	gpio_init_callback(&g_touch0_cb, touch0_isr, BIT(g_touch0.pin));
	err = gpio_add_callback(g_touch0.port, &g_touch0_cb);
	if (err) {
		LOG_ERR("touch0 callback failed (%d)", err);
		return err;
	}

	LOG_INF("touch0 ready: port=%s pin=%d flags=0x%x", g_touch0.port->name, g_touch0.pin,
		g_touch0.dt_flags);
	return 0;
#else
	LOG_WRN("touch0 alias not found; touch input disabled");
	return -ENODEV;
#endif
}
