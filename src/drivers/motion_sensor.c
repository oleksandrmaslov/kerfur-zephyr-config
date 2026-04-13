#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "drivers/motion_sensor.h"

LOG_MODULE_REGISTER(motion_sensor, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_HAS_ALIAS(motion0)
#define MOTION_SENSOR_NODE DT_ALIAS(motion0)
static const struct device *const g_sensor_dev = DEVICE_DT_GET(MOTION_SENSOR_NODE);
#endif

#if DT_HAS_ALIAS(motion0) && DT_ON_BUS(MOTION_SENSOR_NODE, i2c) && \
	DT_NODE_HAS_COMPAT(MOTION_SENSOR_NODE, st_lsm6dso)
#define MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND 1
#include "../../modules/hal/st/sensor/stmemsc/lsm6dso_STdC/driver/lsm6dso_reg.h"
static const struct i2c_dt_spec g_motion_sensor_i2c = I2C_DT_SPEC_GET(MOTION_SENSOR_NODE);
#else
#define MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND 0
#endif

struct motion_sensor_runtime {
	bool idle_trigger_enabled;
	bool gyro_enabled;
	bool hw_step_counter_ready;
#if MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND
	stmdev_ctx_t hw_step_ctx;
#endif
};

static struct motion_sensor_capabilities g_caps;
static struct motion_sensor_runtime g_runtime;
static enum motion_sensor_mode g_mode = MOTION_SENSOR_MODE_DISABLED;
static motion_sensor_event_handler_t g_event_handler;
static void *g_event_user_data;

static int motion_sensor_configure_idle_trigger(bool enable);

#if MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND
static int32_t motion_sensor_hw_step_i2c_read(void *handle, uint8_t reg_addr,
					      uint8_t *value, uint16_t len)
{
	const struct i2c_dt_spec *i2c = handle;

	return i2c_burst_read_dt(i2c, reg_addr, value, len);
}

static int32_t motion_sensor_hw_step_i2c_write(void *handle, uint8_t reg_addr,
					       const uint8_t *value, uint16_t len)
{
	const struct i2c_dt_spec *i2c = handle;

	return i2c_burst_write_dt(i2c, reg_addr, value, len);
}

static void motion_sensor_hw_step_delay(uint32_t millisec)
{
	k_msleep(millisec);
}

static int motion_sensor_enable_hw_step_counter(void)
{
	lsm6dso_emb_sens_t embedded_sensors = { 0 };
	int ret;

	ret = lsm6dso_embedded_sens_get(&g_runtime.hw_step_ctx, &embedded_sensors);
	if (ret < 0) {
		memset(&embedded_sensors, 0, sizeof(embedded_sensors));
	}

	embedded_sensors.step = PROPERTY_ENABLE;
	ret = lsm6dso_embedded_sens_set(&g_runtime.hw_step_ctx, &embedded_sensors);
	if (ret < 0) {
		return -EIO;
	}

	return 0;
}

static int motion_sensor_init_hw_step_backend(void)
{
	int ret;
	uint8_t debounce_steps = 5U;

	if (!device_is_ready(g_motion_sensor_i2c.bus)) {
		return -ENODEV;
	}

	memset(&g_runtime.hw_step_ctx, 0, sizeof(g_runtime.hw_step_ctx));
	g_runtime.hw_step_ctx.read_reg = motion_sensor_hw_step_i2c_read;
	g_runtime.hw_step_ctx.write_reg = motion_sensor_hw_step_i2c_write;
	g_runtime.hw_step_ctx.mdelay = motion_sensor_hw_step_delay;
	g_runtime.hw_step_ctx.handle = (void *)&g_motion_sensor_i2c;

	ret = motion_sensor_enable_hw_step_counter();
	if (ret < 0) {
		return ret;
	}

	ret = lsm6dso_pedo_sens_set(&g_runtime.hw_step_ctx, LSM6DSO_FALSE_STEP_REJ_ADV_MODE);
	if (ret < 0) {
		return -EIO;
	}

	ret = lsm6dso_pedo_debounce_steps_set(&g_runtime.hw_step_ctx, &debounce_steps);
	if (ret < 0) {
		return -EIO;
	}

	ret = lsm6dso_steps_reset(&g_runtime.hw_step_ctx);
	if (ret < 0) {
		return -EIO;
	}

	g_runtime.hw_step_counter_ready = true;
	return 0;
}
#endif

#if DT_HAS_ALIAS(motion0)
static int64_t sensor_value_as_micro(const struct sensor_value *value)
{
	return ((int64_t)value->val1 * 1000000LL) + value->val2;
}

static int16_t sensor_value_to_mg(const struct sensor_value *value)
{
	const int64_t micro_ms2 = sensor_value_as_micro(value);
	const int64_t mg = (micro_ms2 * 1000LL) / SENSOR_G;

	return (int16_t)CLAMP(mg, INT16_MIN, INT16_MAX);
}

static int16_t sensor_value_to_mdps(const struct sensor_value *value)
{
	const int32_t ten_u_degrees = sensor_rad_to_10udegrees(value);
	const int32_t mdps = ten_u_degrees / 100;

	return (int16_t)CLAMP(mdps, INT16_MIN, INT16_MAX);
}

static int sensor_set_odr(enum sensor_channel channel, int frequency_hz)
{
#if DT_HAS_ALIAS(motion0)
	struct sensor_value frequency = {
		.val1 = frequency_hz,
		.val2 = 0,
	};

	return sensor_attr_set(g_sensor_dev, channel, SENSOR_ATTR_SAMPLING_FREQUENCY, &frequency);
#else
	ARG_UNUSED(channel);
	ARG_UNUSED(frequency_hz);
	return -ENODEV;
#endif
}

static void motion_sensor_dispatch_trigger(const struct device *dev,
					   const struct sensor_trigger *trigger)
{
	uint32_t events = MOTION_SENSOR_EVENT_WAKE;

	ARG_UNUSED(dev);

	if (g_event_handler == NULL) {
		return;
	}

	if (trigger->type == SENSOR_TRIG_TILT) {
		events |= MOTION_SENSOR_EVENT_TILT;
	} else if (trigger->type == SENSOR_TRIG_DATA_READY) {
		events |= MOTION_SENSOR_EVENT_DATA_READY;
	}

	g_event_handler(events, g_event_user_data);
}

static int motion_sensor_configure_idle_trigger(bool enable)
{
#if DT_HAS_ALIAS(motion0)
	static const struct sensor_trigger tilt_trigger = {
		.type = SENSOR_TRIG_TILT,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	int err;

	if (!g_caps.backend_ready) {
		return -ENODEV;
	}

	if (!g_caps.backend_has_tilt) {
		g_runtime.idle_trigger_enabled = false;
		return 0;
	}

	if (enable && (g_event_handler == NULL)) {
		enable = false;
	}

	if (g_runtime.idle_trigger_enabled == enable) {
		return 0;
	}

	err = sensor_trigger_set(g_sensor_dev, &tilt_trigger,
				 enable ? motion_sensor_dispatch_trigger : NULL);
	if (err == -ENOTSUP) {
		g_caps.backend_has_tilt = false;
		g_runtime.idle_trigger_enabled = false;
		LOG_WRN("Motion tilt trigger not supported by backend");
		return 0;
	}
	if (err != 0) {
		LOG_WRN("Motion tilt trigger setup failed (%d)", err);
		return err;
	}

	g_runtime.idle_trigger_enabled = enable;
	return 0;
#endif
}

static int motion_sensor_apply_mode(enum motion_sensor_mode mode)
{
	int accel_hz;
	int gyro_hz = 0;
	int err;

	switch (mode) {
	case MOTION_SENSOR_MODE_DISABLED:
		accel_hz = 0;
		break;
	case MOTION_SENSOR_MODE_IDLE:
		accel_hz = CONFIG_KERFUR_MOTION_IDLE_ODR_HZ;
		break;
	case MOTION_SENSOR_MODE_ACTIVE:
		accel_hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;
		break;
	case MOTION_SENSOR_MODE_WALK_MAINTAIN:
		accel_hz = g_runtime.hw_step_counter_ready ?
			CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ :
			MAX(CONFIG_KERFUR_MOTION_IDLE_ODR_HZ,
			    CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ / 2);
		break;
	case MOTION_SENSOR_MODE_IN_HAND_TRACK:
		accel_hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;
		gyro_hz = MAX(CONFIG_KERFUR_MOTION_IDLE_ODR_HZ,
			      CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ / 2);
		break;
	default:
		return -EINVAL;
	}

	err = sensor_set_odr(SENSOR_CHAN_ACCEL_XYZ, accel_hz);
	if ((err != 0) && (err != -ENOTSUP)) {
		LOG_WRN("Motion accel ODR set failed (%d)", err);
		return err;
	}

	err = sensor_set_odr(SENSOR_CHAN_GYRO_XYZ, gyro_hz);
	if ((err != 0) && (err != -ENOTSUP) && (err != -EINVAL)) {
		LOG_WRN("Motion gyro ODR set failed (%d)", err);
		return err;
	}

	g_runtime.gyro_enabled = gyro_hz > 0;
	err = motion_sensor_configure_idle_trigger(mode == MOTION_SENSOR_MODE_IDLE);
	if (err != 0) {
		return err;
	}

#if MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND
	if (g_runtime.hw_step_counter_ready) {
		err = motion_sensor_enable_hw_step_counter();
		if (err != 0) {
			LOG_WRN("Motion hw step refresh failed (%d)", err);
		}
	}
#endif

	g_mode = mode;
	return 0;
}
#else
static int motion_sensor_configure_idle_trigger(bool enable)
{
	ARG_UNUSED(enable);
	return -ENODEV;
}

static int motion_sensor_apply_mode(enum motion_sensor_mode mode)
{
	ARG_UNUSED(mode);
	return -ENODEV;
}
#endif

int motion_sensor_init(void)
{
	memset(&g_caps, 0, sizeof(g_caps));
	memset(&g_runtime, 0, sizeof(g_runtime));
	g_mode = MOTION_SENSOR_MODE_DISABLED;

#if !DT_HAS_ALIAS(motion0)
	LOG_INF("Motion sensor disabled: no motion0 devicetree alias");
	return -ENODEV;
#else
	int hw_step_err = -ENOTSUP;

	if (!device_is_ready(g_sensor_dev)) {
		LOG_WRN("Motion sensor device is not ready");
		return -ENODEV;
	}

	if (DT_NODE_HAS_COMPAT(MOTION_SENSOR_NODE, st_lsm6dso)) {
		g_caps.has_hw_step_counter = true;
		g_caps.has_significant_motion = true;
		g_caps.has_tilt = true;
		g_caps.has_stationary_motion = true;
		g_caps.has_fsm = true;
		g_caps.has_mlc = true;
	}

	g_caps.backend_ready = true;
	g_caps.backend_has_tilt = DT_NODE_HAS_PROP(MOTION_SENSOR_NODE, irq_gpios) &&
				  IS_ENABLED(CONFIG_LSM6DSO_TRIGGER) &&
				  IS_ENABLED(CONFIG_LSM6DSO_TILT);
#if MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND
	hw_step_err = motion_sensor_init_hw_step_backend();
	g_caps.backend_has_hw_step_counter = hw_step_err == 0;
#else
	hw_step_err = -ENOTSUP;
	g_caps.backend_has_hw_step_counter = false;
#endif
	g_caps.backend_has_significant_motion = false;
	g_caps.backend_has_stationary_motion = false;
	g_caps.backend_has_fsm = false;
	g_caps.backend_has_mlc = false;

	if (g_caps.has_hw_step_counter && !g_caps.backend_has_hw_step_counter) {
		LOG_WRN("Motion hw step backend unavailable (%d)", hw_step_err);
	}

	LOG_INF("Motion sensor ready backend_tilt=%d backend_hw_steps=%d silicon_tilt=%d silicon_hw_steps=%d",
		g_caps.backend_has_tilt ? 1 : 0,
		g_caps.backend_has_hw_step_counter ? 1 : 0,
		g_caps.has_tilt ? 1 : 0,
		g_caps.has_hw_step_counter ? 1 : 0);
	return 0;
#endif
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

	if (g_mode == MOTION_SENSOR_MODE_IDLE) {
		return motion_sensor_configure_idle_trigger(handler != NULL);
	}

	return 0;
}

int motion_sensor_set_mode(enum motion_sensor_mode mode)
{
	if (!g_caps.backend_ready) {
		g_mode = mode;
		return -ENODEV;
	}

	if (mode == g_mode) {
		return motion_sensor_configure_idle_trigger(mode == MOTION_SENSOR_MODE_IDLE);
	}

	return motion_sensor_apply_mode(mode);
}

int motion_sensor_fetch_sample(struct motion_sensor_sample *out_sample)
{
#if DT_HAS_ALIAS(motion0)
	struct sensor_value accel[3];
	struct sensor_value gyro[3];
	int err;

	if (out_sample == NULL) {
		return -EINVAL;
	}
	if (!g_caps.backend_ready) {
		return -ENODEV;
	}

	memset(out_sample, 0, sizeof(*out_sample));

	err = sensor_sample_fetch(g_sensor_dev);
	if (err != 0) {
		return err;
	}

	err = sensor_channel_get(g_sensor_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (err != 0) {
		return err;
	}

	out_sample->accel_mg_x = sensor_value_to_mg(&accel[0]);
	out_sample->accel_mg_y = sensor_value_to_mg(&accel[1]);
	out_sample->accel_mg_z = sensor_value_to_mg(&accel[2]);
	out_sample->timestamp_ms = k_uptime_get();

	err = sensor_channel_get(g_sensor_dev, SENSOR_CHAN_GYRO_XYZ, gyro);
	if (err == 0) {
		out_sample->gyro_mdps_x = sensor_value_to_mdps(&gyro[0]);
		out_sample->gyro_mdps_y = sensor_value_to_mdps(&gyro[1]);
		out_sample->gyro_mdps_z = sensor_value_to_mdps(&gyro[2]);
		out_sample->gyro_valid = g_runtime.gyro_enabled;
	}

	return 0;
#else
	ARG_UNUSED(out_sample);
	return -ENODEV;
#endif
}

int motion_sensor_read_hw_step_counter(uint16_t *out_counter)
{
	if (out_counter == NULL) {
		return -EINVAL;
	}

	*out_counter = 0U;
	if (!g_runtime.hw_step_counter_ready) {
		return -ENOTSUP;
	}

#if MOTION_SENSOR_HAS_LSM6DSO_STEP_BACKEND
	if (lsm6dso_number_of_steps_get(&g_runtime.hw_step_ctx, out_counter) < 0) {
		return -EIO;
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}
