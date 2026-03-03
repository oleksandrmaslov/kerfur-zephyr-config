#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/event_bus.h"
#include "drivers/mock_inputs.h"

LOG_MODULE_REGISTER(mock_inputs, CONFIG_LOG_DEFAULT_LEVEL);

#define SHORT_PRESS_DEBOUNCE_MS 30
#define LONG_PRESS_MS 1200

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static const struct gpio_dt_spec g_button0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback g_button0_cb;
static bool g_button0_pressed;
static int64_t g_button0_press_ts;
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
static const struct gpio_dt_spec g_button1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback g_button1_cb;
#endif

static uint8_t g_periodic_mock_phase;

static bool gpio_is_active(const struct gpio_dt_spec *spec)
{
	int value = gpio_pin_get_dt(spec);

	if (value < 0) {
		return false;
	}

	if ((spec->dt_flags & GPIO_ACTIVE_LOW) != 0U) {
		return value == 0;
	}

	return value != 0;
}

static void publish_with_best_effort(enum app_event_type type)
{
	if (app_event_publish(type, 0) != 0) {
		LOG_WRN("Dropped event %s (queue full)", app_event_type_str(type));
	}
}

static void tick_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	publish_with_best_effort(APP_EVENT_TICK_1S);

	if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS)) {
		LOG_DBG("Tick 1s");
	}
}

K_TIMER_DEFINE(g_tick_timer, tick_timer_handler, NULL);

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
static void mock_timer_handler(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);
	g_periodic_mock_phase++;

	if ((g_periodic_mock_phase % 2U) == 0U) {
		publish_with_best_effort(APP_EVENT_MOCK_NOTIFICATION);
		LOG_INF("Mock timer -> MOCK_NOTIFICATION");
	} else {
		publish_with_best_effort(APP_EVENT_MOCK_SHAKE);
		LOG_INF("Mock timer -> MOCK_SHAKE");
	}
}

K_TIMER_DEFINE(g_mock_timer, mock_timer_handler, NULL);
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static void button0_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	bool pressed;
	int64_t now_ms;
	int64_t held_ms;

	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	pressed = gpio_is_active(&g_button0);
	now_ms = k_uptime_get();

	if (pressed) {
		g_button0_pressed = true;
		g_button0_press_ts = now_ms;
		return;
	}

	if (!g_button0_pressed) {
		return;
	}

	g_button0_pressed = false;
	held_ms = now_ms - g_button0_press_ts;
	if (held_ms < SHORT_PRESS_DEBOUNCE_MS) {
		return;
	}

	publish_with_best_effort(APP_EVENT_USER_BUTTON_PRESS);
	if (held_ms >= LONG_PRESS_MS) {
		publish_with_best_effort(APP_EVENT_MOCK_NOTIFICATION);
		LOG_INF("sw0 long press (%lld ms) -> MOCK_NOTIFICATION", (long long)held_ms);
	} else {
		publish_with_best_effort(APP_EVENT_MOCK_PET);
		LOG_INF("sw0 short press (%lld ms) -> MOCK_PET", (long long)held_ms);
	}
}
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
static void button1_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (gpio_is_active(&g_button1)) {
		publish_with_best_effort(APP_EVENT_MOCK_SHAKE);
		LOG_INF("sw1 active -> MOCK_SHAKE");
	}
}
#endif

int mock_inputs_init(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
	int err;
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
	int err2;
#endif

	k_timer_start(&g_tick_timer, K_NO_WAIT, K_SECONDS(1));

#if CONFIG_KERFUR_ENABLE_MOCK_INPUTS
	/*
	 * Periodic synthetic events keep behavior visibly active before
	 * full sensor stack (touch, IMU, notifications) is integrated.
	 * TODO: Replace periodic synthetic events with real sensor/phone inputs.
	 */
	k_timer_start(&g_mock_timer, K_SECONDS(12), K_SECONDS(18));
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
	if (!gpio_is_ready_dt(&g_button0)) {
		LOG_WRN("sw0 GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&g_button0, GPIO_INPUT);
	if (err) {
		LOG_ERR("sw0 configure failed (%d)", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&g_button0, GPIO_INT_EDGE_BOTH);
	if (err) {
		LOG_ERR("sw0 interrupt config failed (%d)", err);
		return err;
	}

	gpio_init_callback(&g_button0_cb, button0_isr, BIT(g_button0.pin));
	err = gpio_add_callback(g_button0.port, &g_button0_cb);
	if (err) {
		LOG_ERR("sw0 callback failed (%d)", err);
		return err;
	}

	LOG_INF("sw0 ready: port=%s pin=%d flags=0x%x", g_button0.port->name, g_button0.pin,
		g_button0.dt_flags);
#else
	LOG_WRN("sw0 alias not found; button short/long press disabled");
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
	if (!gpio_is_ready_dt(&g_button1)) {
		LOG_WRN("sw1 GPIO not ready");
		return -ENODEV;
	}

	err2 = gpio_pin_configure_dt(&g_button1, GPIO_INPUT);
	if (err2) {
		LOG_ERR("sw1 configure failed (%d)", err2);
		return err2;
	}

	err2 = gpio_pin_interrupt_configure_dt(&g_button1, GPIO_INT_EDGE_TO_ACTIVE);
	if (err2) {
		LOG_ERR("sw1 interrupt config failed (%d)", err2);
		return err2;
	}

	gpio_init_callback(&g_button1_cb, button1_isr, BIT(g_button1.pin));
	err2 = gpio_add_callback(g_button1.port, &g_button1_cb);
	if (err2) {
		LOG_ERR("sw1 callback failed (%d)", err2);
		return err2;
	}

	LOG_INF("sw1 ready: port=%s pin=%d flags=0x%x", g_button1.port->name, g_button1.pin,
		g_button1.dt_flags);
#endif

	LOG_INF("Mock inputs initialized");
	return 0;
}
