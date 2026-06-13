/*
 * Azoteq IQS7222A capacitive touch driver (app-level).
 *
 * Protocol facts from the datasheet + Azoteq example (assets/iqs7222a_*):
 *   - 7-bit I2C addr (default 0x44), comms gated by an active-low RDY line.
 *   - 8-bit register addresses for status (0x00 info/version, 0x10..0x17 data,
 *     0xD0 CONTROL_SETTINGS); 16-bit addresses for config (0x8000+).
 *   - Writing register pointer 0xFF "forces" the device to open a comms window
 *     when RDY is not currently asserted.
 *   - TOUCH_EVENT_STATES (per-channel touch bitmask) is at 0x13; reading the
 *     0x10..0x17 block also yields SYSTEM_STATUS (reset / ATI flags).
 *
 * Bring-up follows Azoteq's sequence: verify product -> ensure reset state
 * (SW reset if needed) -> push config -> ACK reset -> re-ATI -> wait for ATI to
 * finish -> enable event mode. Per-RDY reads then stream the touch bitmask into
 * the shared touch_gestures engine; a detected device reset re-runs bring-up.
 *
 * Blocking steps (k_msleep during reset/ATI) run on a dedicated workqueue so
 * they never stall the system workqueue.
 *
 * TODO(hw): a tuned config export for the Kerfur flex is still required for
 * correct sensitivity (see kerfur_iqs7222a_config.c). With an empty config the
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

/* Registers */
#define IQS_REG_PROD_NUM      0x00U
#define IQS_REG_INFOFLAGS     0x10U   /* start of the 0x10..0x17 data block */
#define IQS_REG_TOUCH_STATES  0x13U
#define IQS_REG_CONTROL       0xD0U   /* CONTROL_SETTINGS (2 bytes) */
#define IQS_REG_FORCE_COMMS   0xFFU

/* CONTROL_SETTINGS byte-0 bits */
#define IQS_CTRL_ACK_RESET    BIT(0)
#define IQS_CTRL_SW_RESET     BIT(1)
#define IQS_CTRL_RE_ATI       BIT(2)
#define IQS_CTRL_IF_SELECT_0  BIT(6)
#define IQS_CTRL_IF_SELECT_1  BIT(7)

/* SYSTEM_STATUS (INFOFLAGS byte 0) bits */
#define IQS_INFO_ATI_ACTIVE   BIT(0)
#define IQS_INFO_DEVICE_RESET BIT(3)

#define IQS_PRODUCT_NUM       840U
#define IQS_POLL_MS           40
#define IQS_CFG_CHUNK_MAX     30U
#define IQS_ATI_WAIT_TRIES    200   /* x10ms = up to 2s */
#define IQS_ATI_WAIT_STEP_MS  10

#define IQS_CHANNELS CONFIG_KERFUR_TOUCH_IQS7222A_CHANNELS

#if DT_HAS_ALIAS(touch_iqs0)
#define HAVE_IQS 1
static const struct i2c_dt_spec g_i2c = I2C_DT_SPEC_GET(DT_ALIAS(touch_iqs0));
static const struct gpio_dt_spec g_rdy =
	GPIO_DT_SPEC_GET(DT_ALIAS(touch_iqs0), rdy_gpios);
static struct gpio_callback g_rdy_cb;

K_THREAD_STACK_DEFINE(g_iqs_wq_stack, 2048);
static struct k_work_q g_iqs_wq;
static struct k_work_delayable g_work;
static atomic_t g_rdy_event;
static uint16_t g_last_mask;
static bool g_running;   /* bring-up done, reads active */
#else
#define HAVE_IQS 0
#endif

#if HAVE_IQS

/* True when the RDY window is open (RDY asserted). rdy-gpios uses ACTIVE_LOW,
 * so the logical level reads 1 when the line is physically low / ready. */
static bool iqs_window_open(void)
{
	return gpio_pin_get_dt(&g_rdy) > 0;
}

static int iqs_force_comms(void)
{
	uint8_t reg = IQS_REG_FORCE_COMMS;

	return i2c_write_dt(&g_i2c, &reg, 1);
}

/* Opens a comms window first if the device is not already asserting RDY. */
static void iqs_ensure_window(void)
{
	if (!iqs_window_open()) {
		(void)iqs_force_comms();
	}
}

static int iqs_read8(uint8_t reg, uint8_t *buf, uint32_t len)
{
	return i2c_burst_read_dt(&g_i2c, reg, buf, len);
}

static int iqs_write8(uint8_t reg, const uint8_t *data, uint8_t len)
{
	uint8_t buf[1 + 8];

	if (len > sizeof(buf) - 1) {
		return -EINVAL;
	}
	buf[0] = reg;
	memcpy(&buf[1], data, len);
	return i2c_write_dt(&g_i2c, buf, (uint32_t)(1U + len));
}

static int iqs_write16(uint16_t addr, const uint8_t *data, uint8_t len)
{
	uint8_t buf[2 + IQS_CFG_CHUNK_MAX];

	if (len > IQS_CFG_CHUNK_MAX) {
		return -EINVAL;
	}
	buf[0] = (uint8_t)(addr >> 8);
	buf[1] = (uint8_t)(addr & 0xFFU);
	memcpy(&buf[2], data, len);
	return i2c_write_dt(&g_i2c, buf, (uint32_t)(2U + len));
}

/* Read-modify-write the CONTROL_SETTINGS byte 0, preserving the other bits. */
static int iqs_control_modify(uint8_t set_mask, uint8_t clear_mask)
{
	uint8_t cs[2];
	int err;

	iqs_ensure_window();
	err = iqs_read8(IQS_REG_CONTROL, cs, sizeof(cs));
	if (err != 0) {
		return err;
	}
	cs[0] = (uint8_t)((cs[0] & ~clear_mask) | set_mask);
	iqs_ensure_window();
	return iqs_write8(IQS_REG_CONTROL, cs, sizeof(cs));
}

static bool iqs_info_bit(uint8_t bit_mask)
{
	uint8_t info[2];

	iqs_ensure_window();
	if (iqs_read8(IQS_REG_INFOFLAGS, info, sizeof(info)) != 0) {
		return false;
	}
	return (info[0] & bit_mask) != 0U;
}

static void iqs_push_config(void)
{
	if (kerfur_iqs7222a_config_count == 0U) {
		LOG_WRN("IQS7222A config empty: running on power-up defaults "
			"(touch untuned). Provide the Azoteq export.");
		return;
	}
	for (size_t i = 0U; i < kerfur_iqs7222a_config_count; i++) {
		const struct iqs7222a_config_block *b = &kerfur_iqs7222a_config[i];
		int err;

		iqs_ensure_window();
		err = iqs_write16(b->addr, b->data, b->len);
		if (err != 0) {
			LOG_ERR("IQS7222A config block %u (addr 0x%04x) failed (%d)",
				(unsigned int)i, b->addr, err);
			return;
		}
	}
	LOG_INF("IQS7222A config pushed (%u blocks)",
		(unsigned int)kerfur_iqs7222a_config_count);
}

/* Full bring-up handshake. Returns 0 on success. May block (k_msleep) and so
 * must run on the dedicated IQS workqueue or at init, never the system wq. */
static int iqs_bringup(void)
{
	uint8_t pn[2];
	uint16_t product;
	int tries;

	iqs_ensure_window();
	if (iqs_read8(IQS_REG_PROD_NUM, pn, sizeof(pn)) != 0) {
		LOG_WRN("IQS7222A not responding");
		return -EIO;
	}
	product = (uint16_t)pn[0] | ((uint16_t)pn[1] << 8);
	if (product != IQS_PRODUCT_NUM) {
		LOG_WRN("IQS7222A unexpected product %u (want %u)", product,
			IQS_PRODUCT_NUM);
	} else {
		LOG_INF("IQS7222A detected (product %u)", product);
	}

	/* Force a known reset state so the config loads cleanly. */
	if (!iqs_info_bit(IQS_INFO_DEVICE_RESET)) {
		(void)iqs_control_modify(IQS_CTRL_SW_RESET, 0U);
		k_msleep(100);
	}

	iqs_push_config();
	(void)iqs_control_modify(IQS_CTRL_ACK_RESET, 0U);
	(void)iqs_control_modify(IQS_CTRL_RE_ATI, 0U);

	for (tries = 0; tries < IQS_ATI_WAIT_TRIES; tries++) {
		if (!iqs_info_bit(IQS_INFO_ATI_ACTIVE)) {
			break;
		}
		k_msleep(IQS_ATI_WAIT_STEP_MS);
	}
	if (tries >= IQS_ATI_WAIT_TRIES) {
		LOG_WRN("IQS7222A ATI did not complete in time");
	}

	/* Event mode: IF_SELECT = 0b01 (interrupt on events). */
	(void)iqs_control_modify(IQS_CTRL_IF_SELECT_0, IQS_CTRL_IF_SELECT_1);

	LOG_INF("IQS7222A bring-up complete");
	return 0;
}

static void iqs_work_handler(struct k_work *work)
{
	int64_t now_ms = k_uptime_get();

	ARG_UNUSED(work);

	if (atomic_cas(&g_rdy_event, 1, 0)) {
		uint8_t data[8]; /* 0x10..0x17: status, events, prox, touch */

		if (!iqs_window_open()) {
			(void)iqs_force_comms();
		}
		if (iqs_read8(IQS_REG_INFOFLAGS, data, sizeof(data)) == 0) {
			if (data[0] & IQS_INFO_DEVICE_RESET) {
				LOG_WRN("IQS7222A reset detected; re-initializing");
				g_running = false;
				(void)iqs_bringup();
				g_running = true;
				g_last_mask = 0U;
			} else {
				uint16_t raw = (uint16_t)data[6] |
					       ((uint16_t)data[7] << 8);

				g_last_mask = raw &
					(uint16_t)((1U << IQS_CHANNELS) - 1U);
			}
		}
	}

	touch_gestures_update(g_last_mask, now_ms);

	if (g_last_mask != 0U || touch_gestures_pending()) {
		(void)k_work_reschedule_for_queue(&g_iqs_wq, &g_work,
						  K_MSEC(IQS_POLL_MS));
	}
}

static void iqs_rdy_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!g_running) {
		return; /* ignore RDY pulses during bring-up */
	}
	atomic_set(&g_rdy_event, 1);
	(void)k_work_reschedule_for_queue(&g_iqs_wq, &g_work, K_NO_WAIT);
}
#endif /* HAVE_IQS */

int iqs7222a_init(void)
{
#if HAVE_IQS
	struct touch_gesture_config cfg;
	int err;

	if (!device_is_ready(g_i2c.bus)) {
		LOG_WRN("IQS7222A I2C bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&g_rdy)) {
		LOG_WRN("IQS7222A RDY GPIO not ready");
		return -ENODEV;
	}

	touch_gestures_default_config(&cfg);
	cfg.channel_count = IQS_CHANNELS;
	touch_gestures_init(&cfg, touch_input_publish_gesture, NULL, k_uptime_get());

	k_work_queue_init(&g_iqs_wq);
	k_work_queue_start(&g_iqs_wq, g_iqs_wq_stack,
			   K_THREAD_STACK_SIZEOF(g_iqs_wq_stack),
			   K_PRIO_PREEMPT(8), NULL);
	k_work_init_delayable(&g_work, iqs_work_handler);

	err = gpio_pin_configure_dt(&g_rdy, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY configure failed (%d)", err);
		return err;
	}

	/* Bring up before enabling the RDY interrupt so reads only start once
	 * the device is configured and in event mode. */
	err = iqs_bringup();
	if (err != 0) {
		return err;
	}

	gpio_init_callback(&g_rdy_cb, iqs_rdy_isr, BIT(g_rdy.pin));
	err = gpio_add_callback(g_rdy.port, &g_rdy_cb);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY callback failed (%d)", err);
		return err;
	}
	/* Mark running before enabling the interrupt so the first RDY edge after
	 * bring-up is not dropped by the ISR's g_running guard. */
	g_running = true;
	err = gpio_pin_interrupt_configure_dt(&g_rdy, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		LOG_ERR("IQS7222A RDY interrupt config failed (%d)", err);
		g_running = false;
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
