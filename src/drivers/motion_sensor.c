#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "drivers/motion_sensor.h"

LOG_MODULE_REGISTER(motion_sensor, CONFIG_LOG_DEFAULT_LEVEL);

#define MOTION0_NODE DT_ALIAS(motion0)

struct motion_sensor_runtime {
	const struct device *dev;
	struct motion_sensor_capabilities caps;
	enum motion_sensor_mode mode;
	motion_sensor_event_handler_t event_handler;
	void *event_user_data;
	bool tilt_trigger_enabled;
};

static struct motion_sensor_runtime g_motion;

#if DT_HAS_ALIAS(motion0)
static const struct device *const g_motion_dev = DEVICE_DT_GET(MOTION0_NODE);
#endif

static int configure_tilt_trigger(bool enable);

static void motion_tilt_trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);

	if (g_motion.event_handler == NULL) {
		return;
	}

	g_motion.event_handler(MOTION_SENSOR_EVENT_WAKE | MOTION_SENSOR_EVENT_TILT,
			       g_motion.event_user_data);
}

static int set_accel_frequency(int hz)
{
	struct sensor_value odr = {
		.val1 = hz,
		.val2 = 0,
	};

	if (!g_motion.caps.backend_ready) {
		return -ENODEV;
	}

	return sensor_attr_set(g_motion.dev, SENSOR_CHAN_ACCEL_XYZ,
			       SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
}

static int disable_gyro_runtime(void)
{
	struct sensor_value odr = {
		.val1 = 0,
		.val2 = 0,
	};

	if (!g_motion.caps.backend_ready) {
		return -ENODEV;
	}

	return sensor_attr_set(g_motion.dev, SENSOR_CHAN_GYRO_XYZ,
			       SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
}

static int configure_tilt_trigger(bool enable)
{
#if DT_HAS_ALIAS(motion0) && defined(CONFIG_LSM6DSO_TILT)
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_TILT,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	int err;

	if (!g_motion.caps.backend_ready || !g_motion.caps.backend_has_tilt) {
		return -ENOTSUP;
	}

	err = sensor_trigger_set(g_motion.dev, &trig,
				 enable ? motion_tilt_trigger_handler : NULL);
	if (err == 0) {
		g_motion.tilt_trigger_enabled = enable;
	}

	return err;
#else
	ARG_UNUSED(enable);
	return -ENOTSUP;
#endif
}

static int32_t rad_to_mdps(const struct sensor_value *value)
{
	return sensor_rad_to_10udegrees(value) / 100;
}

int motion_sensor_init(void)
{
	memset(&g_motion, 0, sizeof(g_motion));
	g_motion.mode = MOTION_SENSOR_MODE_DISABLED;

#if DT_HAS_ALIAS(motion0)
	g_motion.caps.silicon_has_hw_step_counter = true;
	g_motion.caps.silicon_has_significant_motion = true;
	g_motion.caps.silicon_has_tilt = true;
	g_motion.caps.silicon_has_stationary_motion = true;
	g_motion.caps.silicon_has_fsm = true;
	g_motion.caps.silicon_has_mlc = true;

	if (!device_is_ready(g_motion_dev)) {
		LOG_WRN("motion0 present but not ready");
		return -ENODEV;
	}

	g_motion.dev = g_motion_dev;
	g_motion.caps.backend_ready = true;
	g_motion.caps.backend_has_tilt =
#if defined(CONFIG_LSM6DSO_TILT)
		DT_NODE_HAS_PROP(MOTION0_NODE, irq_gpios);
#else
		false;
#endif
	g_motion.caps.backend_has_hw_step_counter = false;
	g_motion.caps.backend_has_significant_motion = false;
	g_motion.caps.backend_has_stationary_motion = false;
	g_motion.caps.backend_has_fsm = false;
	g_motion.caps.backend_has_mlc = false;

	(void)disable_gyro_runtime();
	(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_IDLE);
	LOG_INF("Motion sensor ready via motion0");
	return 0;
#else
	LOG_INF("No motion0 alias; motion sensor disabled");
	return -ENODEV;
#endif
}

bool motion_sensor_is_ready(void)
{
	return g_motion.caps.backend_ready;
}

enum motion_sensor_mode motion_sensor_get_mode(void)
{
	return g_motion.mode;
}

const struct motion_sensor_capabilities *motion_sensor_get_capabilities(void)
{
	return &g_motion.caps;
}

int motion_sensor_set_event_handler(motion_sensor_event_handler_t handler, void *user_data)
{
	g_motion.event_handler = handler;
	g_motion.event_user_data = user_data;

	if (g_motion.mode == MOTION_SENSOR_MODE_IDLE) {
		(void)configure_tilt_trigger(handler != NULL);
	}

	return 0;
}

int motion_sensor_set_mode(enum motion_sensor_mode mode)
{
	int err = 0;
	int target_hz = 0;
	bool want_tilt_trigger = false;

	if (!g_motion.caps.backend_ready) {
		g_motion.mode = MOTION_SENSOR_MODE_DISABLED;
		return -ENODEV;
	}

	switch (mode) {
	case MOTION_SENSOR_MODE_DISABLED:
		target_hz = 0;
		break;
	case MOTION_SENSOR_MODE_IDLE:
		target_hz = CONFIG_KERFUR_MOTION_IDLE_ODR_HZ;
		want_tilt_trigger = g_motion.event_handler != NULL;
		break;
	case MOTION_SENSOR_MODE_ACTIVE:
	case MOTION_SENSOR_MODE_WALK_MAINTAIN:
	case MOTION_SENSOR_MODE_IN_HAND_TRACK:
		target_hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;
		break;
	default:
		return -EINVAL;
	}

	if (g_motion.tilt_trigger_enabled && !want_tilt_trigger) {
		(void)configure_tilt_trigger(false);
	}

	err = set_accel_frequency(target_hz);
	if (err != 0) {
		LOG_WRN("Failed to set motion accel ODR %dHz (%d)", target_hz, err);
	}

	if (want_tilt_trigger) {
		(void)configure_tilt_trigger(true);
	}

	g_motion.mode = mode;
	return err;
}

int motion_sensor_fetch_sample(struct motion_sensor_sample *out_sample)
{
	struct sensor_value accel[3];
	struct sensor_value gyro[3];
	int err;

	if (out_sample == NULL) {
		return -EINVAL;
	}
	if (!g_motion.caps.backend_ready) {
		return -ENODEV;
	}

	memset(out_sample, 0, sizeof(*out_sample));

	err = sensor_sample_fetch(g_motion.dev);
	if (err != 0) {
		return err;
	}

	err = sensor_channel_get(g_motion.dev, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (err != 0) {
		return err;
	}

	out_sample->accel_mg_x = (int16_t)CLAMP(sensor_ms2_to_mg(&accel[0]), INT16_MIN, INT16_MAX);
	out_sample->accel_mg_y = (int16_t)CLAMP(sensor_ms2_to_mg(&accel[1]), INT16_MIN, INT16_MAX);
	out_sample->accel_mg_z = (int16_t)CLAMP(sensor_ms2_to_mg(&accel[2]), INT16_MIN, INT16_MAX);
	out_sample->timestamp_ms = k_uptime_get();

	err = sensor_channel_get(g_motion.dev, SENSOR_CHAN_GYRO_XYZ, gyro);
	if (err == 0) {
		out_sample->gyro_valid = true;
		out_sample->gyro_mdps_x = (int16_t)CLAMP(rad_to_mdps(&gyro[0]), INT16_MIN, INT16_MAX);
		out_sample->gyro_mdps_y = (int16_t)CLAMP(rad_to_mdps(&gyro[1]), INT16_MIN, INT16_MAX);
		out_sample->gyro_mdps_z = (int16_t)CLAMP(rad_to_mdps(&gyro[2]), INT16_MIN, INT16_MAX);
	}

	return 0;
}

int motion_sensor_read_hw_step_counter(uint16_t *out_counter)
{
	if (out_counter == NULL) {
		return -EINVAL;
	}

	*out_counter = 0U;
	if (!g_motion.caps.backend_ready) {
		return -ENODEV;
	}

	return -ENOTSUP;
}
