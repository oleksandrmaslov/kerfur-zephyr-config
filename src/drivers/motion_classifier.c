#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "core/event_bus.h"
#include "drivers/in_hand_detector.h"
#include "drivers/motion_classifier.h"
#include "drivers/motion_sensor.h"

LOG_MODULE_REGISTER(motion_classifier, CONFIG_LOG_DEFAULT_LEVEL);

enum motion_classifier_mode {
	MOTION_CLASSIFIER_MODE_OFF = 0,
	MOTION_CLASSIFIER_MODE_IDLE,
	MOTION_CLASSIFIER_MODE_ACTIVE_WINDOW,
	MOTION_CLASSIFIER_MODE_WALK_MAINTAIN,
	MOTION_CLASSIFIER_MODE_IN_HAND_TRACK,
};

struct motion_classifier_state {
	struct k_work_delayable sample_work;
	struct in_hand_detector in_hand_detector;
	enum motion_classifier_mode mode;
	struct motion_sensor_sample last_sample;
	bool enabled;
	bool initialized;
	bool debug_logging_enabled;
	bool battery_low;
	bool battery_critical;
	bool battery_percent_known;
	bool charging;
	bool gravity_valid;
	bool hw_counter_valid;
	bool walking_active;
	bool in_hand;
	bool over_step_threshold;
	int8_t battery_percent;
	int16_t gravity_x;
	int16_t gravity_y;
	int16_t gravity_z;
	int16_t look_target_x;
	int16_t look_target_y;
	uint8_t walking_confidence;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t look_confidence;
	uint16_t last_hw_step_counter;
	uint8_t recent_step_count;
	uint8_t recent_step_index;
	uint32_t pending_steps;
	int64_t active_until_ms;
	int64_t last_peak_ms;
	int64_t recent_step_ms[6];
	int64_t last_motion_wake_ms;
	int64_t suppress_look_until_ms;
	int64_t last_carry_publish_ms;
	int64_t last_look_publish_ms;
	int64_t last_debug_log_ms;
	int64_t last_pickup_timestamp_ms;
	int64_t last_in_hand_timestamp_ms;
	int64_t last_motion_sample_timestamp_ms;
	int64_t last_still_timestamp_ms;
	bool last_published_in_hand;
	uint8_t last_published_pickup_confidence;
	uint8_t last_published_in_hand_confidence;
	uint8_t last_published_walking_confidence;
	int16_t last_published_look_x;
	int16_t last_published_look_y;
	uint8_t last_published_look_confidence;
};

static struct motion_classifier_state g_motion;

static uint8_t clamp_u8_0_100(int value)
{
	if (value < 0) {
		return 0U;
	}
	if (value > 100) {
		return 100U;
	}
	return (uint8_t)value;
}

static int16_t clamp_look_value(int value)
{
	return (int16_t)CLAMP(value, -100, 100);
}

static int abs_i16(int16_t value)
{
	return (value < 0) ? -value : value;
}

static int64_t max_i64(int64_t lhs, int64_t rhs)
{
	return (lhs > rhs) ? lhs : rhs;
}

static int sample_period_ms(void)
{
	int rate_hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;

	if (rate_hz <= 0) {
		return 100;
	}

	return MAX(20, 1000 / rate_hz);
}

static void motion_classifier_schedule_now(void)
{
	(void)k_work_reschedule(&g_motion.sample_work, K_NO_WAIT);
}

static void motion_classifier_set_mode(enum motion_classifier_mode mode, int64_t now_ms)
{
	enum motion_sensor_mode sensor_mode;

	g_motion.mode = mode;

	switch (mode) {
	case MOTION_CLASSIFIER_MODE_OFF:
		(void)k_work_cancel_delayable(&g_motion.sample_work);
		(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_DISABLED);
		return;
	case MOTION_CLASSIFIER_MODE_IDLE:
		(void)k_work_cancel_delayable(&g_motion.sample_work);
		(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_IDLE);
		return;
	case MOTION_CLASSIFIER_MODE_ACTIVE_WINDOW:
		sensor_mode = MOTION_SENSOR_MODE_ACTIVE;
		break;
	case MOTION_CLASSIFIER_MODE_WALK_MAINTAIN:
		sensor_mode = MOTION_SENSOR_MODE_WALK_MAINTAIN;
		break;
	case MOTION_CLASSIFIER_MODE_IN_HAND_TRACK:
		sensor_mode = MOTION_SENSOR_MODE_IN_HAND_TRACK;
		break;
	default:
		return;
	}

	(void)motion_sensor_set_mode(sensor_mode);
	g_motion.active_until_ms = max_i64(g_motion.active_until_ms, now_ms);
	motion_classifier_schedule_now();
}

static void publish_carry_state(int64_t now_ms, bool force)
{
	const bool changed = force ||
		(g_motion.last_published_in_hand != g_motion.in_hand) ||
		(g_motion.last_published_pickup_confidence != g_motion.pickup_confidence) ||
		(g_motion.last_published_in_hand_confidence != g_motion.in_hand_confidence) ||
		(g_motion.last_published_walking_confidence != g_motion.walking_confidence);

	if (!changed && ((now_ms - g_motion.last_carry_publish_ms) < 250LL)) {
		return;
	}

	(void)app_event_publish_carry_state_with_timestamp(g_motion.in_hand,
							    g_motion.pickup_confidence,
							    g_motion.in_hand_confidence,
							    g_motion.walking_confidence,
							    now_ms);
	g_motion.last_carry_publish_ms = now_ms;
	g_motion.last_published_in_hand = g_motion.in_hand;
	g_motion.last_published_pickup_confidence = g_motion.pickup_confidence;
	g_motion.last_published_in_hand_confidence = g_motion.in_hand_confidence;
	g_motion.last_published_walking_confidence = g_motion.walking_confidence;
}

static void publish_look_target(int64_t now_ms, bool force)
{
	const int min_interval_ms = MAX(50, 1000 / CONFIG_KERFUR_MOTION_GAZE_RATE_HZ);
	const bool changed = force ||
		(abs_i16(g_motion.look_target_x - g_motion.last_published_look_x) >= 3) ||
		(abs_i16(g_motion.look_target_y - g_motion.last_published_look_y) >= 3) ||
		(g_motion.look_confidence != g_motion.last_published_look_confidence);

	if (!changed && ((now_ms - g_motion.last_look_publish_ms) < min_interval_ms)) {
		return;
	}

	(void)app_event_publish_look_target_with_timestamp(g_motion.look_target_x,
							   g_motion.look_target_y,
							   g_motion.look_confidence,
							   now_ms);
	g_motion.last_look_publish_ms = now_ms;
	g_motion.last_published_look_x = g_motion.look_target_x;
	g_motion.last_published_look_y = g_motion.look_target_y;
	g_motion.last_published_look_confidence = g_motion.look_confidence;
}

static void flush_step_batch(int64_t now_ms)
{
	if (g_motion.pending_steps == 0U) {
		return;
	}

	(void)app_event_publish_step_batch_with_timestamp((int32_t)g_motion.pending_steps,
							 g_motion.last_hw_step_counter,
							 g_motion.walking_confidence,
							 false,
							 now_ms);
	g_motion.pending_steps = 0U;
}

static void update_gravity_estimate(const struct motion_sensor_sample *sample)
{
	if (!g_motion.gravity_valid) {
		g_motion.gravity_x = sample->accel_mg_x;
		g_motion.gravity_y = sample->accel_mg_y;
		g_motion.gravity_z = sample->accel_mg_z;
		g_motion.gravity_valid = true;
		return;
	}

	g_motion.gravity_x += (sample->accel_mg_x - g_motion.gravity_x) / 6;
	g_motion.gravity_y += (sample->accel_mg_y - g_motion.gravity_y) / 6;
	g_motion.gravity_z += (sample->accel_mg_z - g_motion.gravity_z) / 6;
}

static uint8_t count_recent_steps(int64_t now_ms)
{
	uint8_t count = 0U;
	uint8_t index;

	for (index = 0U; index < ARRAY_SIZE(g_motion.recent_step_ms); index++) {
		if ((g_motion.recent_step_ms[index] > 0LL) &&
		    ((now_ms - g_motion.recent_step_ms[index]) <= 2200LL)) {
			count++;
		}
	}

	return count;
}

static void note_step(int64_t now_ms)
{
	g_motion.pending_steps++;
	g_motion.recent_step_ms[g_motion.recent_step_index] = now_ms;
	g_motion.recent_step_index =
		(g_motion.recent_step_index + 1U) % ARRAY_SIZE(g_motion.recent_step_ms);
	g_motion.recent_step_count = MIN((uint8_t)ARRAY_SIZE(g_motion.recent_step_ms),
					 (uint8_t)(g_motion.recent_step_count + 1U));

	if (g_motion.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
		flush_step_batch(now_ms);
	}
}

static void update_hw_step_counter(int64_t now_ms)
{
	uint16_t hw_counter;
	int err;
	uint16_t delta;

	err = motion_sensor_read_hw_step_counter(&hw_counter);
	if (err != 0) {
		return;
	}

	if (!g_motion.hw_counter_valid) {
		g_motion.hw_counter_valid = true;
		g_motion.last_hw_step_counter = hw_counter;
		return;
	}

	if (hw_counter >= g_motion.last_hw_step_counter) {
		delta = hw_counter - g_motion.last_hw_step_counter;
	} else {
		delta = (uint16_t)(UINT16_MAX - g_motion.last_hw_step_counter + hw_counter + 1U);
	}

	if (delta > 128U) {
		g_motion.last_hw_step_counter = hw_counter;
		return;
	}

	if (delta > 0U) {
		g_motion.pending_steps += delta;
		if (g_motion.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
			(void)app_event_publish_step_batch_with_timestamp((int32_t)g_motion.pending_steps,
								 hw_counter,
								 g_motion.walking_confidence,
								 true,
								 now_ms);
			g_motion.pending_steps = 0U;
		}
	}

	g_motion.last_hw_step_counter = hw_counter;
}

static bool detect_software_step(int16_t linear_x, int16_t linear_y, int16_t linear_z,
				 bool rough_motion, int64_t now_ms)
{
	const uint16_t intensity = (uint16_t)(abs_i16(linear_x) + abs_i16(linear_y) +
					       abs_i16(linear_z));
	const bool over_threshold = intensity >= 260U;
	const int64_t since_last_peak = now_ms - g_motion.last_peak_ms;
	bool step_detected = false;

	if (!g_motion.over_step_threshold && over_threshold && !rough_motion &&
	    ((g_motion.last_peak_ms == 0LL) ||
	     ((since_last_peak >= 250LL) && (since_last_peak <= 900LL)))) {
		step_detected = true;
		g_motion.last_peak_ms = now_ms;
		note_step(now_ms);
	}

	g_motion.over_step_threshold = over_threshold;
	return step_detected;
}

static void update_walking_confidence(bool step_detected, bool rough_motion, int64_t now_ms)
{
	const uint8_t recent_steps = count_recent_steps(now_ms);

	if (step_detected) {
		if (recent_steps >= 4U) {
			g_motion.walking_confidence =
				clamp_u8_0_100(g_motion.walking_confidence + 16);
		} else if (recent_steps >= 2U) {
			g_motion.walking_confidence =
				clamp_u8_0_100(g_motion.walking_confidence + 10);
		} else {
			g_motion.walking_confidence =
				clamp_u8_0_100(g_motion.walking_confidence + 6);
		}
	} else if (rough_motion) {
		g_motion.walking_confidence =
			clamp_u8_0_100(g_motion.walking_confidence - 18);
	} else if ((g_motion.last_peak_ms > 0LL) &&
		   ((now_ms - g_motion.last_peak_ms) > 1400LL)) {
		g_motion.walking_confidence =
			clamp_u8_0_100(g_motion.walking_confidence - 8);
	} else {
		g_motion.walking_confidence =
			clamp_u8_0_100(g_motion.walking_confidence - 2);
	}

	if (!g_motion.walking_active &&
	    (g_motion.walking_confidence >= CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD)) {
		g_motion.walking_active = true;
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_START,
						       g_motion.walking_confidence,
						       now_ms);
	} else if (g_motion.walking_active &&
		   (g_motion.walking_confidence <= CONFIG_KERFUR_MOTION_WALK_STOP_THRESHOLD)) {
		g_motion.walking_active = false;
		flush_step_batch(now_ms);
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_STOP,
						       g_motion.walking_confidence,
						       now_ms);
	}
}

static int16_t move_towards(int16_t current, int16_t target, int16_t max_delta)
{
	int delta = target - current;

	if (delta > max_delta) {
		delta = max_delta;
	} else if (delta < -max_delta) {
		delta = -max_delta;
	}

	return current + (int16_t)delta;
}

static void update_look_target(const struct in_hand_detector_output *detector_out, int64_t now_ms)
{
	int16_t raw_x;
	int16_t raw_y;
	int scale;

	if ((now_ms < g_motion.suppress_look_until_ms) ||
	    !detector_out->in_hand || g_motion.walking_active ||
	    (detector_out->look_confidence < 35U)) {
		g_motion.look_target_x = move_towards(g_motion.look_target_x, 0, 8);
		g_motion.look_target_y = move_towards(g_motion.look_target_y, 0, 8);
		g_motion.look_confidence = 0U;
		return;
	}

	raw_x = clamp_look_value((-g_motion.gravity_x * 100) / 700);
	raw_y = clamp_look_value((g_motion.gravity_y * 100) / 700);

	if (abs_i16(raw_x) <= 8) {
		raw_x = 0;
	}
	if (abs_i16(raw_y) <= 8) {
		raw_y = 0;
	}

	scale = detector_out->look_confidence;
	raw_x = (int16_t)((raw_x * scale) / 100);
	raw_y = (int16_t)((raw_y * scale) / 100);

	g_motion.look_target_x = move_towards(g_motion.look_target_x, raw_x, 10);
	g_motion.look_target_y = move_towards(g_motion.look_target_y, raw_y, 10);
	g_motion.look_confidence = detector_out->look_confidence;
}

static void emit_detector_events(const struct in_hand_detector_output *detector_out, int64_t now_ms)
{
	if (detector_out->pickup_candidate) {
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKUP_CANDIDATE,
						       detector_out->pickup_confidence,
						       now_ms);
	}
	if (detector_out->picked_up) {
		g_motion.last_pickup_timestamp_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKED_UP,
						       detector_out->pickup_confidence,
						       now_ms);
	}
	if (detector_out->in_hand_enter) {
		g_motion.last_in_hand_timestamp_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_ENTER,
						       detector_out->in_hand_confidence,
						       now_ms);
	}
	if (detector_out->in_hand_exit) {
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_EXIT,
						       detector_out->in_hand_confidence,
						       now_ms);
	}
}

static void log_motion_debug(int64_t now_ms)
{
	if (!g_motion.debug_logging_enabled ||
	    ((now_ms - g_motion.last_debug_log_ms) < 1000LL)) {
		return;
	}

	LOG_INF("Motion conf walk=%u active=%d pickup=%u in_hand=%u look=%u target=%d,%d mode=%d batt=%d low=%d",
		g_motion.walking_confidence,
		g_motion.walking_active ? 1 : 0,
		g_motion.pickup_confidence,
		g_motion.in_hand_confidence,
		g_motion.look_confidence,
		g_motion.look_target_x,
		g_motion.look_target_y,
		g_motion.mode,
		g_motion.battery_percent,
		g_motion.battery_low ? 1 : 0);
	g_motion.last_debug_log_ms = now_ms;
}

static void motion_classifier_sample_work(struct k_work *work)
{
	struct motion_sensor_sample sample;
	struct in_hand_detector_input detector_in;
	struct in_hand_detector_output detector_out;
	int16_t linear_x;
	int16_t linear_y;
	int16_t linear_z;
	uint16_t motion_mg;
	uint16_t smooth_motion_mg;
	int err;
	bool rough_motion;
	bool step_detected = false;
	int64_t now_ms;

	ARG_UNUSED(work);

	if (!g_motion.enabled) {
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_OFF, k_uptime_get());
		return;
	}

	if (g_motion.battery_critical) {
		g_motion.walking_confidence = 0U;
		g_motion.walking_active = false;
		g_motion.pickup_confidence = 0U;
		g_motion.in_hand_confidence = 0U;
		g_motion.look_confidence = 0U;
		g_motion.look_target_x = move_towards(g_motion.look_target_x, 0, 12);
		g_motion.look_target_y = move_towards(g_motion.look_target_y, 0, 12);
		publish_carry_state(k_uptime_get(), true);
		publish_look_target(k_uptime_get(), true);
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_IDLE, k_uptime_get());
		return;
	}

	err = motion_sensor_fetch_sample(&sample);
	if (err != 0) {
		LOG_WRN("Motion sample fetch failed (%d)", err);
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_IDLE, k_uptime_get());
		return;
	}

	now_ms = sample.timestamp_ms;
	g_motion.last_motion_sample_timestamp_ms = now_ms;
	update_gravity_estimate(&sample);
	update_hw_step_counter(now_ms);

	linear_x = sample.accel_mg_x - g_motion.gravity_x;
	linear_y = sample.accel_mg_y - g_motion.gravity_y;
	linear_z = sample.accel_mg_z - g_motion.gravity_z;
	motion_mg = (uint16_t)(abs_i16(linear_x) + abs_i16(linear_y) + abs_i16(linear_z));
	smooth_motion_mg = (uint16_t)(abs_i16(sample.accel_mg_x - g_motion.last_sample.accel_mg_x) +
				      abs_i16(sample.accel_mg_y - g_motion.last_sample.accel_mg_y) +
				      abs_i16(sample.accel_mg_z - g_motion.last_sample.accel_mg_z));
	rough_motion = (motion_mg >= 900U) ||
		      (sample.gyro_valid &&
		       ((abs_i16(sample.gyro_mdps_x) + abs_i16(sample.gyro_mdps_y) +
			 abs_i16(sample.gyro_mdps_z)) >= 60000));

	if (rough_motion) {
		g_motion.suppress_look_until_ms = now_ms + 1200LL;
		if (motion_mg >= 1200U) {
			(void)app_event_publish_with_timestamp(APP_EVENT_SHAKE_ROUGH, 0, now_ms);
		} else {
			(void)app_event_publish_with_timestamp(APP_EVENT_SHAKE_LIGHT, 0, now_ms);
		}
	}

	if (!motion_sensor_get_capabilities()->backend_has_hw_step_counter) {
		step_detected = detect_software_step(linear_x, linear_y, linear_z, rough_motion, now_ms);
	}
	update_walking_confidence(step_detected, rough_motion, now_ms);

	memset(&detector_in, 0, sizeof(detector_in));
	detector_in.gravity_x = g_motion.gravity_x;
	detector_in.gravity_y = g_motion.gravity_y;
	detector_in.gravity_z = g_motion.gravity_z;
	detector_in.motion_mg = motion_mg;
	detector_in.smooth_motion_mg = smooth_motion_mg;
	detector_in.walking_confidence = g_motion.walking_confidence;
	detector_in.walking_active = g_motion.walking_active;
	detector_in.rough_motion = rough_motion;
	detector_in.motion_wake = ((now_ms - g_motion.last_motion_wake_ms) <= 1200LL);
	detector_in.now_ms = now_ms;

	in_hand_detector_process(&g_motion.in_hand_detector, &detector_in, &detector_out);

	g_motion.pickup_confidence = detector_out.pickup_confidence;
	g_motion.in_hand_confidence = detector_out.in_hand_confidence;
	g_motion.in_hand = detector_out.in_hand;
	if (detector_out.state == IN_HAND_DETECTOR_SURFACE_STILL) {
		g_motion.last_still_timestamp_ms = now_ms;
	}

	emit_detector_events(&detector_out, now_ms);
	update_look_target(&detector_out, now_ms);
	publish_carry_state(now_ms, detector_out.pickup_candidate || detector_out.picked_up ||
				       detector_out.in_hand_enter || detector_out.in_hand_exit);
	publish_look_target(now_ms, false);

	g_motion.last_sample = sample;
	log_motion_debug(now_ms);

	if (g_motion.walking_active) {
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_WALK_MAINTAIN, now_ms + 1000LL);
	} else if (g_motion.in_hand || (g_motion.in_hand_confidence >= 45U)) {
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_IN_HAND_TRACK,
					   now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS);
	} else if (now_ms < g_motion.active_until_ms) {
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_ACTIVE_WINDOW,
					   g_motion.active_until_ms);
	} else {
		motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_IDLE, now_ms);
		return;
	}

	(void)k_work_reschedule(&g_motion.sample_work, K_MSEC(sample_period_ms()));
}

static void motion_sensor_event_handler(uint32_t events, void *user_data)
{
	int64_t now_ms = k_uptime_get();

	ARG_UNUSED(user_data);

	if (!g_motion.enabled || g_motion.battery_critical) {
		return;
	}

	g_motion.last_motion_wake_ms = now_ms;
	g_motion.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
	(void)app_event_publish_with_timestamp(APP_EVENT_MOTION_WAKE, (int32_t)events, now_ms);
	motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_ACTIVE_WINDOW, g_motion.active_until_ms);
}

int motion_classifier_init(int64_t now_ms)
{
	int err;

	memset(&g_motion, 0, sizeof(g_motion));
	k_work_init_delayable(&g_motion.sample_work, motion_classifier_sample_work);
	in_hand_detector_init(&g_motion.in_hand_detector, now_ms);
	g_motion.mode = MOTION_CLASSIFIER_MODE_OFF;
	g_motion.battery_percent = -1;
	g_motion.last_published_look_confidence = UINT8_MAX;

	if (!IS_ENABLED(CONFIG_KERFUR_MOTION)) {
		LOG_INF("Motion classifier disabled by Kconfig");
		return 0;
	}

	err = motion_sensor_init();
	if (err != 0) {
		LOG_WRN("Motion classifier running without sensor backend (%d)", err);
		g_motion.initialized = true;
		g_motion.enabled = false;
		return 0;
	}

	(void)motion_sensor_set_event_handler(motion_sensor_event_handler, NULL);
	g_motion.initialized = true;
	g_motion.enabled = true;
	motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_IDLE, now_ms);
	return 0;
}

void motion_classifier_on_event(const struct app_event *event, const struct pet_state *state)
{
	if ((event == NULL) || !g_motion.initialized) {
		return;
	}

	switch (event->type) {
	case APP_EVENT_BATTERY_PERCENT_UPDATE:
		g_motion.battery_percent = event->payload.battery_percent.known ?
			event->payload.battery_percent.percent : -1;
		g_motion.battery_percent_known = event->payload.battery_percent.known;
		g_motion.battery_low = event->payload.battery_percent.known &&
			(event->payload.battery_percent.percent <= 20);
		g_motion.battery_critical = event->payload.battery_percent.known &&
			(event->payload.battery_percent.percent <= 5);
		if (g_motion.battery_low && (g_motion.active_until_ms > 0LL)) {
			g_motion.active_until_ms = MIN(g_motion.active_until_ms,
						       event->timestamp_ms + 1500LL);
		}
		break;
	case APP_EVENT_BATTERY_LOW:
		g_motion.battery_low = true;
		break;
	case APP_EVENT_BATTERY_CRITICAL:
		g_motion.battery_critical = true;
		break;
	case APP_EVENT_CHARGER_CONNECTED:
		g_motion.charging = true;
		break;
	case APP_EVENT_CHARGER_DISCONNECTED:
		g_motion.charging = false;
		break;
	case APP_EVENT_WALKING_START:
		g_motion.walking_active = true;
		g_motion.walking_confidence = (event->param > 0) ?
			clamp_u8_0_100(event->param) : CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD;
		g_motion.active_until_ms = event->timestamp_ms + 1500LL;
		break;
	case APP_EVENT_WALKING_STOP:
		g_motion.walking_active = false;
		g_motion.walking_confidence = clamp_u8_0_100(event->param);
		break;
	case APP_EVENT_CARRY_STATE_UPDATE:
		g_motion.in_hand = event->payload.carry_state.in_hand;
		g_motion.pickup_confidence = event->payload.carry_state.pickup_confidence;
		g_motion.in_hand_confidence = event->payload.carry_state.in_hand_confidence;
		g_motion.walking_confidence = event->payload.carry_state.walking_confidence;
		break;
	case APP_EVENT_PICKED_UP:
		g_motion.last_pickup_timestamp_ms = event->timestamp_ms;
		g_motion.active_until_ms = event->timestamp_ms +
					   CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
		break;
	case APP_EVENT_IN_HAND_ENTER:
		g_motion.last_in_hand_timestamp_ms = event->timestamp_ms;
		g_motion.in_hand = true;
		break;
	case APP_EVENT_IN_HAND_EXIT:
		g_motion.in_hand = false;
		break;
	default:
		break;
	}

	if ((state != NULL) && !g_motion.enabled) {
		g_motion.battery_low = state->battery_low;
		g_motion.battery_critical = state->battery_critical;
	}
}

void motion_classifier_set_debug_logging(bool enabled)
{
	g_motion.debug_logging_enabled = enabled;
	if (enabled) {
		g_motion.last_debug_log_ms = 0LL;
	}
}

bool motion_classifier_is_debug_logging(void)
{
	return g_motion.debug_logging_enabled;
}

bool motion_classifier_is_enabled(void)
{
	return g_motion.enabled;
}
