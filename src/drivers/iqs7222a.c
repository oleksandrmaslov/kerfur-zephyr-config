/*
 * Azoteq IQS7222A capacitive touch driver (app-level, RDY-gated).
 *
 * Register/protocol facts from the Azoteq datasheet + example code
 * (assets/iqs7222a_*): product number 840 at reg 0x00; per-channel touch state
 * is a bitmask at reg 0x13 (TOUCH_EVENT_STATES, CH0..CH9 + Hall); config lives
 * at 16-bit addresses 0x8000+; the device pulses RDY when data is ready.
 *
 * This driver verifies the part, optionally pushes the Kerfur config blob, then
 * reads 0x13 on each RDY assertion and feeds the channel bitmask into the
 * shared touch_gestures engine. Gestures are mapped to APP_EVENT_USER_* events
 * by touch_input_publish_gesture(), identical to the GPIO front-end.
 *
 * TODO(hw): full reset/ATI sequencing and a tuned config blob are required for
 * correct sensitivity (see kerfur_iqs7222a_config.c). Without the config the
 * chip runs on power-up defaults.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "drivers/iqs7222a.h"
#include "drivers/kerfur_iqs7222a_config.h"
#include "drivers/touch_gestures.h"
#include "drivers/touch_input.h"

LOG_MODULE_REGISTER(iqs7222a, CONFIG_LOG_DEFAULT_LEVEL);

#define IQS7222A_REG_PROD_NUM        0x00U
#define IQS7222A_REG_TOUCH_STATES    0x13U
#define IQS7222A_PRODUCT_NUM         840U

#define IQS7222A_POLL_MS             40
#define IQS7222A_CONFIG_CHUNK_MAX    30U

#define IQS_CHANNELS CONFIG_KERFUR_TOUCH_IQS7222A_CHANNELS

#if DT_HAS_ALIAS(touch_iqs0)
#define HAVE_IQS 1
static const struct i2c_dt_spec g_i2c = I2C_DT_SPEC_GET(DT_ALIAS(touch_iqs0));
static const struct gpio_dt_spec g_rdy =
	GPIO_DT_SPEC_GET(DT_ALIAS(touch_iqs0), rdy_gpios);
static struct gpio_callback g_rdy_cb;
static struct k_work_delayable g_work;
static atomic_t g_rdy_event;
static uint16_t g_last_mask;
#else
#define HAVE_IQS 0
#endif

#if HAVE_IQS
static int iqs_read_touch_mask(uint16_t *out_mask)
{
	uint8_t buf[2];
	int err = i2c_burst_read_dt(&g_i2c, IQS7222A_REG_TOUCH_STATES, buf, sizeof(buf));

	if (err != 0) {
		return err;
	}
	/* Keep only the petting channels; ignore Hall / unused upper bits. */
	*out_mask = (uint16_t)(((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) &
			       (uint16_t)((1U << IQS_CHANNELS) - 1U));
	return 0;
}

static int iqs_write_config_block(const struct iqs7222a_config_block *b)
{
	uint8_t buf[2 + IQS7222A_CONFIG_CHUNK_MAX];

	if (b->len > IQS7222A_CONFIG_CHUNK_MAX) {
		return -EINVAL;
	}
	buf[0] = (uint8_t)(b->addr >> 8);
	buf[1] = (uint8_t)(b->addr & 0xFFU);
	memcpy(&buf[2], b->data, b->len);
	return i2c_write_dt(&g_i2c, buf, (uint32_t)(2U + b->len));
}

static void iqs_push_config(void)
{
	if (kerfur_iqs7222a_config_count == 0U) {
		LOG_WRN("IQS7222A config empty: running on power-up defaults "
			"(touch untuned). Provide the Azoteq export.");
		return;
	}
	for (size_t i = 0U; i < kerfur_iqs7222a_config_count; i++) {
		int err = iqs_write_config_block(&kerfur_iqs7222a_config[i]);

		if (err != 0) {
			LOG_ERR("IQS7222A config block %u (addr 0x%04x) failed (%d)",
				(unsigned int)i, kerfur_iqs7222a_config[i].addr, err);
			return;
		}
	}
	LOG_INF("IQS7222A config pushed (%u blocks)",
		(unsigned int)kerfur_iqs7222a_config_count);
}

static void iqs_work_handler(struct k_work *work)
{
	int64_t now_ms = k_uptime_get();

	ARG_UNUSED(work);

	/* Only touch the bus inside an RDY window; between events reuse the
	 * cached mask so the gesture engine's timers still advance. */
	if (atomic_cas(&g_rdy_event, 1, 0)) {
		uint16_t mask;

		if (iqs_read_touch_mask(&mask) == 0) {
			g_last_mask = mask;
		}
	}

	touch_gestures_update(g_last_mask, now_ms);

	if (g_last_mask != 0U || touch_gestures_pending()) {
		(void)k_work_reschedule(&g_work, K_MSEC(IQS7222A_POLL_MS));
	}
}

static void iqs_rdy_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_set(&g_rdy_event, 1);
	(void)k_work_reschedule(&g_work, K_NO_WAIT);
}
#endif /* HAVE_IQS */

int iqs7222a_init(void)
{
#if HAVE_IQS
	struct touch_gesture_config cfg;
	uint8_t pn[2];
	uint16_t product;
	int err;

	if (!device_is_ready(g_i2c.bus)) {
		LOG_WRN("IQS7222A I2C bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&g_rdy)) {
		LOG_WRN("IQS7222A RDY GPIO not ready");
		return -ENODEV;
	}

	err = i2c_burst_read_dt(&g_i2c, IQS7222A_REG_PROD_NUM, pn, sizeof(pn));
	if (err != 0) {
		LOG_WRN("IQS7222A not responding (%d)", err);
		return -EIO;
	}
	product = (uint16_t)pn[0] | ((uint16_t)pn[1] << 8);
	if (product != IQS7222A_PRODUCT_NUM) {
		LOG_WRN("IQS7222A unexpected product number %u (want %u)",
			product, IQS7222A_PRODUCT_NUM);
		/* Continue anyway: byte order / config can still be exercised. */
	} else {
		LOG_INF("IQS7222A detected (product %u)", product);
	}

	iqs_push_config();

	touch_gestures_default_config(&cfg);
	cfg.channel_count = IQS_CHANNELS;
	touch_gestures_init(&cfg, touch_input_publish_gesture, NULL, k_uptime_get());

	err = gpio_pin_configure_dt(&g_rdy, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY configure failed (%d)", err);
		return err;
	}
	/* Register the work item + callback before enabling the interrupt so an
	 * early RDY edge can never hit an uninitialized work item. */
	k_work_init_delayable(&g_work, iqs_work_handler);
	gpio_init_callback(&g_rdy_cb, iqs_rdy_isr, BIT(g_rdy.pin));
	err = gpio_add_callback(g_rdy.port, &g_rdy_cb);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY callback failed (%d)", err);
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(&g_rdy, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY interrupt config failed (%d)", err);
		return err;
	}

	LOG_INF("IQS7222A ready: %u channels on %s@0x%02x, RDY %s pin %d",
		IQS_CHANNELS, g_i2c.bus->name, g_i2c.addr,
		g_rdy.port->name, g_rdy.pin);
	return 0;
#else
	LOG_WRN("touch_iqs0 alias not found; IQS7222A disabled");
	return -ENODEV;
#endif
}
