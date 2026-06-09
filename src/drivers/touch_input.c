/*
 * Touch input front-end.
 *
 * Today this drives a single GPIO touch line (the `touch0` alias) but feeds a
 * hardware-agnostic gesture engine (touch_gestures.c) so the higher layers see
 * real tap / double-tap / long-press / hold / stroke gestures instead of a raw
 * edge. The future IQS7222A driver will feed the same gesture engine with a
 * multi-zone channel mask; only this file changes, not the gesture logic.
 *
 * The line is sampled by a small poller that the edge ISR kicks. The poller
 * keeps running while the line is active or a gesture decision is still pending
 * (so deferred taps / long-press / hold can fire), then stops so we are not
 * polling while idle.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "drivers/touch_gestures.h"
#include "drivers/touch_input.h"

LOG_MODULE_REGISTER(touch_input, CONFIG_LOG_DEFAULT_LEVEL);

#define TOUCH_POLL_MS    40

static void publish_with_best_effort(enum app_event_type type)
{
	if (app_event_publish(type, 0) != 0) {
		LOG_WRN("Dropped event %s (queue full)", app_event_type_str(type));
	}
}

void touch_input_publish_gesture(enum touch_gesture gesture, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("touch gesture: %s", touch_gesture_str(gesture));

	switch (gesture) {
	case TOUCH_GESTURE_TAP:
	case TOUCH_GESTURE_DOUBLE_TAP:
		/* TODO: a dedicated playful double-tap event would let the
		 * behavior engine distinguish it; map to TAP for now. */
		publish_with_best_effort(APP_EVENT_USER_TAP);
		break;
	case TOUCH_GESTURE_STROKE:
		publish_with_best_effort(APP_EVENT_USER_PET_SOFT);
		break;
	case TOUCH_GESTURE_REPEATED_STROKE:
		publish_with_best_effort(APP_EVENT_USER_PET_LONG);
		break;
	case TOUCH_GESTURE_LONG_PRESS:
	case TOUCH_GESTURE_HOLD:
		publish_with_best_effort(APP_EVENT_USER_HOLD);
		break;
	default:
		break;
	}
}

#if DT_NODE_HAS_STATUS(DT_ALIAS(touch0), okay)
static const struct gpio_dt_spec g_touch0 = GPIO_DT_SPEC_GET(DT_ALIAS(touch0), gpios);
static struct gpio_callback g_touch0_cb;
static struct k_work_delayable g_touch_work;

static void touch_poll_handler(struct k_work *work)
{
	int64_t now_ms = k_uptime_get();
	bool level;

	ARG_UNUSED(work);

	level = gpio_pin_get_dt(&g_touch0) > 0;
	touch_gestures_set(level, now_ms);

	/* Keep sampling while touched or while a gesture decision is pending so
	 * deferred taps / long-press / hold can fire; then stop and idle. */
	if (level || touch_gestures_pending()) {
		(void)k_work_reschedule(&g_touch_work, K_MSEC(TOUCH_POLL_MS));
	}
}

static void touch0_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&g_touch_work, K_NO_WAIT);
}
#endif

int touch_input_init(void)
{
	struct touch_gesture_config cfg;

	touch_gestures_default_config(&cfg);
	touch_gestures_init(&cfg, touch_input_publish_gesture, NULL, k_uptime_get());

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

	/* Both edges so we can time press/release for gesture detection. */
	err = gpio_pin_interrupt_configure_dt(&g_touch0, GPIO_INT_EDGE_BOTH);
	if (err) {
		LOG_ERR("touch0 interrupt config failed (%d)", err);
		return err;
	}

	k_work_init_delayable(&g_touch_work, touch_poll_handler);
	gpio_init_callback(&g_touch0_cb, touch0_isr, BIT(g_touch0.pin));
	err = gpio_add_callback(g_touch0.port, &g_touch0_cb);
	if (err) {
		LOG_ERR("touch0 callback failed (%d)", err);
		return err;
	}

	/* If the line is already active at boot there will be no edge to kick
	 * the poller, so start it now. Otherwise we stay idle until the ISR. */
	if (gpio_pin_get_dt(&g_touch0) > 0) {
		(void)k_work_reschedule(&g_touch_work, K_NO_WAIT);
	}

	LOG_INF("touch0 ready (gesture engine): port=%s pin=%d flags=0x%x",
		g_touch0.port->name, g_touch0.pin, g_touch0.dt_flags);
	return 0;
#else
	LOG_WRN("touch0 alias not found; touch input disabled");
	return -ENODEV;
#endif
}
