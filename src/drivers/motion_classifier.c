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

#define MOTION_IDLE_POLL_MS 800
#define MOTION_IDLE_WAKE_THRESHOLD_MG 180U
#define MOTION_IDLE_WAKE_SMOOTH_MG 120U
#define MOTION_FEATURE_WINDOW_FRAMES 12U
struct motion_feature_frame {
	uint16_t motion_mg;
	uint16_t smooth_motion_mg;
	uint16_t jerk_mg;
	uint16_t orientation_delta_mg;
	uint16_t orientation_rate_mg;
	uint32_t gyro_sum_mdps;
	int64_t timestamp_ms;
};

struct motion_feature_summary {
	uint16_t avg_motion_mg;
	uint16_t avg_smooth_motion_mg;
	uint16_t avg_jerk_mg;
	uint16_t avg_orientation_delta_mg;
	uint16_t avg_orientation_rate_mg;
	uint32_t avg_gyro_sum_mdps;
	uint8_t stability_confidence;
	uint8_t chaos_confidence;
	uint8_t cadence_confidence;
};

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
	bool pending_steps_from_hw_counter;
	bool walking_active;
	bool in_hand;
	bool over_step_threshold;
	bool look_reference_valid;
	int8_t battery_percent;
	int16_t gravity_x;
	int16_t gravity_y;
	int16_t gravity_z;
	int16_t last_linear_x;
	int16_t last_linear_y;
	int16_t last_linear_z;
	int16_t look_reference_x;
	int16_t look_reference_y;
	int16_t look_reference_z;
	int16_t look_target_x;
	int16_t look_target_y;
	uint8_t walking_confidence;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t look_confidence;
	uint8_t stability_confidence;
	uint8_t chaos_confidence;
	uint8_t cadence_confidence;
	uint16_t last_hw_step_counter;
	uint8_t recent_step_count;
	uint8_t recent_step_index;
	uint8_t feature_frame_count;
	uint8_t feature_frame_index;
	uint32_t pending_steps;
	int64_t active_until_ms;
	int64_t last_peak_ms;
	int64_t recent_step_ms[6];
	struct motion_feature_frame feature_frames[MOTION_FEATURE_WINDOW_FRAMES];
	int64_t last_motion_wake_ms;
	int64_t suppress_look_until_ms;
	int64_t last_shake_event_ms;
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

static void normalize_motion_sample(struct motion_sensor_sample *sample)
{
	int16_t accel_x;
	int16_t accel_y;
	int16_t accel_z;
	int16_t gyro_x;
	int16_t gyro_y;
	int16_t gyro_z;

	if (sample == NULL) {
		return;
	}

	accel_x = sample->accel_mg_x;
	accel_y = sample->accel_mg_y;
	accel_z = sample->accel_mg_z;
	gyro_x = sample->gyro_mdps_x;
	gyro_y = sample->gyro_mdps_y;
	gyro_z = sample->gyro_mdps_z;

	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_SWAP_XY)) {
		int16_t tmp = accel_x;

		accel_x = accel_y;
		accel_y = tmp;
		tmp = gyro_x;
		gyro_x = gyro_y;
		gyro_y = tmp;
	}

	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_X)) {
		accel_x = -accel_x;
		gyro_x = -gyro_x;
	}
	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_Y)) {
		accel_y = -accel_y;
		gyro_y = -gyro_y;
	}
	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_Z)) {
		accel_z = -accel_z;
		gyro_z = -gyro_z;
	}

	sample->accel_mg_x = accel_x;
	sample->accel_mg_y = accel_y;
	sample->accel_mg_z = accel_z;
	sample->gyro_mdps_x = gyro_x;
	sample->gyro_mdps_y = gyro_y;
	sample->gyro_mdps_z = gyro_z;
}

static bool motion_classifier_uses_idle_polling(void)
{
	const struct motion_sensor_capabilities *caps = motion_sensor_get_capabilities();

	return caps->backend_ready && !caps->backend_has_tilt;
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
		(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_IDLE);
		if (motion_classifier_uses_idle_polling()) {
			(void)k_work_reschedule(&g_motion.sample_work, K_MSEC(MOTION_IDLE_POLL_MS));
		} else {
			(void)k_work_cancel_delayable(&g_motion.sample_work);
		}
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
							 g_motion.pending_steps_from_hw_counter,
							 now_ms);
	g_motion.pending_steps = 0U;
	g_motion.pending_steps_from_hw_counter = false;
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

static void note_recent_step_timestamp(int64_t step_ms)
{
	g_motion.recent_step_ms[g_motion.recent_step_index] = step_ms;
	g_motion.recent_step_index =
		(g_motion.recent_step_index + 1U) % ARRAY_SIZE(g_motion.recent_step_ms);
	g_motion.recent_step_count = MIN((uint8_t)ARRAY_SIZE(g_motion.recent_step_ms),
					 (uint8_t)(g_motion.recent_step_count + 1U));
	g_motion.last_peak_ms = MAX(g_motion.last_peak_ms, step_ms);
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
	g_motion.pending_steps_from_hw_counter = false;
	g_motion.pending_steps++;
	note_recent_step_timestamp(now_ms);

	if (g_motion.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
		flush_step_batch(now_ms);
	}
}

static uint8_t recent_step_time_at(uint8_t logical_index)
{
	return (g_motion.recent_step_index + ARRAY_SIZE(g_motion.recent_step_ms) -
		g_motion.recent_step_count + logical_index) % ARRAY_SIZE(g_motion.recent_step_ms);
}

static uint8_t cadence_confidence_from_steps(int64_t now_ms)
{
	uint8_t recent_steps = count_recent_steps(now_ms);
	uint32_t interval_sum = 0U;
	uint32_t interval_error = 0U;
	uint32_t average_interval_ms;
	uint8_t interval_count = 0U;
	uint8_t index;

	if (recent_steps < 3U || g_motion.recent_step_count < 3U) {
		return 0U;
	}

	for (index = 1U; index < g_motion.recent_step_count; index++) {
		const int64_t previous = g_motion.recent_step_ms[recent_step_time_at(index - 1U)];
		const int64_t current = g_motion.recent_step_ms[recent_step_time_at(index)];
		uint32_t interval_ms;

		if ((previous <= 0LL) || (current <= previous)) {
			continue;
		}

		interval_ms = (uint32_t)(current - previous);
		if ((interval_ms < 250U) || (interval_ms > 1100U)) {
			continue;
		}

		interval_sum += interval_ms;
		interval_count++;
	}

	if (interval_count < 2U) {
		return 0U;
	}

	average_interval_ms = interval_sum / interval_count;
	if ((average_interval_ms < 280U) || (average_interval_ms > 900U)) {
		return 0U;
	}

	for (index = 1U; index < g_motion.recent_step_count; index++) {
		const int64_t previous = g_motion.recent_step_ms[recent_step_time_at(index - 1U)];
		const int64_t current = g_motion.recent_step_ms[recent_step_time_at(index)];
		uint32_t interval_ms;

		if ((previous <= 0LL) || (current <= previous)) {
			continue;
		}

		interval_ms = (uint32_t)(current - previous);
		if ((interval_ms < 250U) || (interval_ms > 1100U)) {
			continue;
		}

		interval_error += (interval_ms >= average_interval_ms) ?
			(interval_ms - average_interval_ms) :
			(average_interval_ms - interval_ms);
	}

	return clamp_u8_0_100((int)(recent_steps * 14U) +
			       26 -
			       (int)(interval_error / MAX(interval_count, 1U) / 4U));
}

static void append_feature_frame(const struct motion_feature_frame *frame)
{
	if (frame == NULL) {
		return;
	}

	g_motion.feature_frames[g_motion.feature_frame_index] = *frame;
	g_motion.feature_frame_index =
		(g_motion.feature_frame_index + 1U) % ARRAY_SIZE(g_motion.feature_frames);
	g_motion.feature_frame_count = MIN((uint8_t)ARRAY_SIZE(g_motion.feature_frames),
					   (uint8_t)(g_motion.feature_frame_count + 1U));
}

static uint8_t recent_feature_frame_at(uint8_t logical_index)
{
	return (g_motion.feature_frame_index + ARRAY_SIZE(g_motion.feature_frames) -
		g_motion.feature_frame_count + logical_index) %
		ARRAY_SIZE(g_motion.feature_frames);
}

static void summarize_motion_features(struct motion_feature_summary *summary, int64_t now_ms)
{
	uint32_t sum_motion = 0U;
	uint32_t sum_smooth = 0U;
	uint32_t sum_jerk = 0U;
	uint32_t sum_orientation_delta = 0U;
	uint32_t sum_orientation_rate = 0U;
	uint64_t sum_gyro = 0U;
	uint8_t stable_samples = 0U;
	uint8_t smooth_samples = 0U;
	uint8_t chaotic_samples = 0U;
	uint8_t count = 0U;
	uint8_t index;

	memset(summary, 0, sizeof(*summary));

	for (index = 0U; index < g_motion.feature_frame_count; index++) {
		const struct motion_feature_frame *frame =
			&g_motion.feature_frames[recent_feature_frame_at(index)];

		if ((now_ms - frame->timestamp_ms) > 1800LL) {
			continue;
		}

		sum_motion += frame->motion_mg;
		sum_smooth += frame->smooth_motion_mg;
		sum_jerk += frame->jerk_mg;
		sum_orientation_delta += frame->orientation_delta_mg;
		sum_orientation_rate += frame->orientation_rate_mg;
		sum_gyro += frame->gyro_sum_mdps;
		count++;

		if ((frame->motion_mg >= 20U) && (frame->motion_mg <= 260U) &&
		    (frame->jerk_mg <= 220U) && (frame->gyro_sum_mdps <= 26000U) &&
		    (frame->orientation_rate_mg <= 220U)) {
			stable_samples++;
		}

		if ((frame->orientation_delta_mg >= 18U) && (frame->orientation_delta_mg <= 240U) &&
		    (frame->jerk_mg <= 240U) && (frame->gyro_sum_mdps <= 30000U)) {
			smooth_samples++;
		}

		if ((frame->motion_mg >= 650U) || (frame->jerk_mg >= 360U) ||
		    (frame->gyro_sum_mdps >= 42000U)) {
			chaotic_samples++;
		}
	}

	if (count == 0U) {
		return;
	}

	summary->avg_motion_mg = (uint16_t)(sum_motion / count);
	summary->avg_smooth_motion_mg = (uint16_t)(sum_smooth / count);
	summary->avg_jerk_mg = (uint16_t)(sum_jerk / count);
	summary->avg_orientation_delta_mg = (uint16_t)(sum_orientation_delta / count);
	summary->avg_orientation_rate_mg = (uint16_t)(sum_orientation_rate / count);
	summary->avg_gyro_sum_mdps = (uint32_t)(sum_gyro / count);
	summary->cadence_confidence = cadence_confidence_from_steps(now_ms);
	summary->stability_confidence = clamp_u8_0_100(
		18 + (int)(stable_samples * 7U) + (int)(smooth_samples * 4U) -
		(int)(chaotic_samples * 8U) -
		MAX(0, (int)summary->avg_jerk_mg - 220) / 5 -
		MAX(0, (int)summary->avg_gyro_sum_mdps - 26000) / 1800);
	summary->chaos_confidence = clamp_u8_0_100(
		(int)(chaotic_samples * 14U) +
		MAX(0, (int)summary->avg_jerk_mg - 170) / 4 +
		MAX(0, (int)summary->avg_gyro_sum_mdps - 20000) / 1500 +
		MAX(0, (int)summary->avg_motion_mg - 280) / 6 -
		(int)(smooth_samples * 3U));
}

static void note_hw_steps(uint16_t delta, int64_t now_ms)
{
	uint16_t index;
	uint32_t spacing_ms = 420U;

	if (delta == 0U) {
		return;
	}

	if ((delta > 1U) && (g_motion.last_peak_ms > 0LL)) {
		spacing_ms = CLAMP((uint32_t)((now_ms - g_motion.last_peak_ms) / delta),
				   280U, 850U);
	}

	for (index = 0U; index < MIN(delta, (uint16_t)ARRAY_SIZE(g_motion.recent_step_ms)); index++) {
		const uint32_t reverse_index = (uint32_t)(delta - 1U - index);
		const int64_t step_ms = now_ms - (reverse_index * spacing_ms);

		note_recent_step_timestamp(step_ms);
	}
}

static uint16_t update_hw_step_counter(int64_t now_ms)
{
	uint16_t hw_counter;
	int err;
	uint16_t delta;

	err = motion_sensor_read_hw_step_counter(&hw_counter);
	if (err != 0) {
		return 0U;
	}

	if (!g_motion.hw_counter_valid) {
		g_motion.hw_counter_valid = true;
		g_motion.last_hw_step_counter = hw_counter;
		return 0U;
	}

	if (hw_counter >= g_motion.last_hw_step_counter) {
		delta = hw_counter - g_motion.last_hw_step_counter;
	} else {
		delta = (uint16_t)(UINT16_MAX - g_motion.last_hw_step_counter + hw_counter + 1U);
	}

	if (delta > 128U) {
		g_motion.last_hw_step_counter = hw_counter;
		return 0U;
	}

	if (delta > 0U) {
		g_motion.pending_steps_from_hw_counter = true;
		g_motion.pending_steps += delta;
		note_hw_steps(delta, now_ms);
		if (g_motion.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
			(void)app_event_publish_step_batch_with_timestamp((int32_t)g_motion.pending_steps,
								 hw_counter,
								 g_motion.walking_confidence,
								 true,
								 now_ms);
			g_motion.pending_steps = 0U;
			g_motion.pending_steps_from_hw_counter = false;
		}
	}

	g_motion.last_hw_step_counter = hw_counter;
	return delta;
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

static void update_walking_confidence(bool step_detected,
				      const struct motion_feature_summary *summary,
				      int64_t now_ms)
{
	const uint8_t recent_steps = count_recent_steps(now_ms);
	const uint8_t cadence_confidence = summary->cadence_confidence;
	const uint8_t chaos_confidence = summary->chaos_confidence;
	int delta = 0;

	if (step_detected) {
		delta += 4;
		delta += cadence_confidence / 10U;
		delta += recent_steps * 2U;
		delta -= chaos_confidence / 12U;
		if (recent_steps >= 4U) {
			delta += 6;
		} else if (recent_steps >= 2U) {
			delta += 3;
		}
	} else if (chaos_confidence >= 75U) {
		delta -= 18;
	} else if ((cadence_confidence >= 60U) && (recent_steps >= 3U)) {
		delta -= 1;
	} else if ((g_motion.last_peak_ms > 0LL) &&
		   ((now_ms - g_motion.last_peak_ms) > 1400LL)) {
		delta -= 8;
	} else {
		delta -= 3;
	}

	if ((summary->avg_motion_mg >= 250U) && (summary->avg_motion_mg <= 520U) &&
	    (chaos_confidence <= 55U) && (cadence_confidence >= 40U)) {
		delta += 2;
	}

	g_motion.walking_confidence =
		clamp_u8_0_100((int)g_motion.walking_confidence + delta);

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

static void publish_motion_reaction(uint16_t motion_mg,
				    uint32_t gyro_sum_mdps,
				    bool stable_in_hand,
				    int64_t now_ms)
{
	enum app_event_type type = APP_EVENT_COUNT;
	int64_t suppress_until_ms = 0LL;
	int cooldown_ms = 0;

	if ((motion_mg >= 1800U) || (gyro_sum_mdps >= 90000U)) {
		type = APP_EVENT_IMPACT;
		suppress_until_ms = now_ms + 1400LL;
		cooldown_ms = 1200;
	} else if ((motion_mg >= 1200U) || (gyro_sum_mdps >= 70000U)) {
		type = APP_EVENT_SHAKE_ROUGH;
		suppress_until_ms = now_ms + 1200LL;
		cooldown_ms = 900;
	} else if ((motion_mg >= 700U) || (gyro_sum_mdps >= 42000U)) {
		type = APP_EVENT_SHAKE_PLAY;
		suppress_until_ms = 0LL;
		cooldown_ms = 750;
	} else if ((motion_mg >= 350U) || (gyro_sum_mdps >= 22000U)) {
		type = APP_EVENT_SHAKE_LIGHT;
		suppress_until_ms = 0LL;
		cooldown_ms = 600;
	}

	if (type == APP_EVENT_COUNT) {
		return;
	}
	if (stable_in_hand &&
	    ((type == APP_EVENT_SHAKE_LIGHT) || (type == APP_EVENT_SHAKE_PLAY))) {
		return;
	}
	if ((now_ms - g_motion.last_shake_event_ms) < cooldown_ms) {
		return;
	}

	g_motion.last_shake_event_ms = now_ms;
	g_motion.suppress_look_until_ms = MAX(g_motion.suppress_look_until_ms, suppress_until_ms);
	(void)app_event_publish_with_timestamp(type, 0, now_ms);
}

static void reset_look_reference(void)
{
	g_motion.look_reference_valid = false;
	g_motion.look_reference_x = 0;
	g_motion.look_reference_y = 0;
	g_motion.look_reference_z = 0;
}

static void capture_look_reference(void)
{
	g_motion.look_reference_x = g_motion.gravity_x;
	g_motion.look_reference_y = g_motion.gravity_y;
	g_motion.look_reference_z = g_motion.gravity_z;
	g_motion.look_reference_valid = true;
}

static void update_look_reference(const struct in_hand_detector_output *detector_out,
				  const struct motion_feature_summary *summary)
{
	if (!detector_out->in_hand) {
		reset_look_reference();
		return;
	}

	if (detector_out->in_hand_enter || !g_motion.look_reference_valid) {
		if ((summary->stability_confidence >= 35U) || detector_out->in_hand_enter) {
			capture_look_reference();
		}
		return;
	}

	if ((summary->stability_confidence >= 55U) &&
	    (summary->chaos_confidence <= 35U) &&
	    (summary->cadence_confidence <= 40U)) {
		g_motion.look_reference_x += (g_motion.gravity_x - g_motion.look_reference_x) / 14;
		g_motion.look_reference_y += (g_motion.gravity_y - g_motion.look_reference_y) / 14;
		g_motion.look_reference_z += (g_motion.gravity_z - g_motion.look_reference_z) / 14;
	}
}

static void update_look_target(const struct in_hand_detector_output *detector_out,
			       const struct motion_feature_summary *summary,
			       int64_t now_ms)
{
	int16_t raw_x;
	int16_t raw_y;
	int confidence;

	if ((now_ms < g_motion.suppress_look_until_ms) ||
	    !detector_out->in_hand || g_motion.walking_active ||
	    (detector_out->look_confidence < 35U) ||
	    !g_motion.look_reference_valid) {
		g_motion.look_target_x = move_towards(g_motion.look_target_x, 0, 8);
		g_motion.look_target_y = move_towards(g_motion.look_target_y, 0, 8);
		g_motion.look_confidence = 0U;
		return;
	}

	raw_x = clamp_look_value((-(g_motion.gravity_x - g_motion.look_reference_x) * 100) /
				 CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);
	raw_y = clamp_look_value(((g_motion.gravity_y - g_motion.look_reference_y) * 100) /
				 CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);

	if (abs_i16(raw_x) <= 6) {
		raw_x = 0;
	}
	if (abs_i16(raw_y) <= 6) {
		raw_y = 0;
	}

	confidence = detector_out->look_confidence -
		(summary->chaos_confidence / 2U) -
		(summary->cadence_confidence / 3U);

	g_motion.look_target_x = move_towards(g_motion.look_target_x, raw_x,
					      g_motion.battery_low ? 6 : 10);
	g_motion.look_target_y = move_towards(g_motion.look_target_y, raw_y,
					      g_motion.battery_low ? 6 : 10);
	g_motion.look_confidence = clamp_u8_0_100(confidence);
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

	LOG_INF("Motion conf walk=%u active=%d pickup=%u in_hand=%u look=%u target=%d,%d stability=%u cadence=%u chaos=%u ref=%d mode=%d batt=%d low=%d",
		g_motion.walking_confidence,
		g_motion.walking_active ? 1 : 0,
		g_motion.pickup_confidence,
		g_motion.in_hand_confidence,
		g_motion.look_confidence,
		g_motion.look_target_x,
		g_motion.look_target_y,
		g_motion.stability_confidence,
		g_motion.cadence_confidence,
		g_motion.chaos_confidence,
		g_motion.look_reference_valid ? 1 : 0,
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
	struct motion_feature_frame feature_frame;
	struct motion_feature_summary feature_summary;
	int16_t linear_x;
	int16_t linear_y;
	int16_t linear_z;
	int16_t previous_gravity_x;
	int16_t previous_gravity_y;
	int16_t previous_gravity_z;
	uint16_t orientation_delta_mg;
	uint16_t orientation_rate_mg;
	uint16_t jerk_mg;
	uint16_t motion_mg;
	uint16_t smooth_motion_mg;
	uint32_t gyro_sum_mdps;
	uint16_t hw_step_delta = 0U;
	int err;
	bool rough_motion;
	bool step_detected = false;
	bool idle_poll = g_motion.mode == MOTION_CLASSIFIER_MODE_IDLE;
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
		g_motion.stability_confidence = 0U;
		g_motion.cadence_confidence = 0U;
		g_motion.chaos_confidence = 0U;
		g_motion.look_target_x = move_towards(g_motion.look_target_x, 0, 12);
		g_motion.look_target_y = move_towards(g_motion.look_target_y, 0, 12);
		reset_look_reference();
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

	normalize_motion_sample(&sample);
	now_ms = sample.timestamp_ms;
	g_motion.last_motion_sample_timestamp_ms = now_ms;
	previous_gravity_x = g_motion.gravity_x;
	previous_gravity_y = g_motion.gravity_y;
	previous_gravity_z = g_motion.gravity_z;
	update_gravity_estimate(&sample);

	if (idle_poll) {
		const uint16_t idle_motion_mg =
			(uint16_t)(abs_i16(sample.accel_mg_x - g_motion.gravity_x) +
				   abs_i16(sample.accel_mg_y - g_motion.gravity_y) +
				   abs_i16(sample.accel_mg_z - g_motion.gravity_z));
		const uint16_t idle_smooth_mg =
			(uint16_t)(abs_i16(sample.accel_mg_x - g_motion.last_sample.accel_mg_x) +
				   abs_i16(sample.accel_mg_y - g_motion.last_sample.accel_mg_y) +
				   abs_i16(sample.accel_mg_z - g_motion.last_sample.accel_mg_z));

		g_motion.last_sample = sample;

		if ((idle_motion_mg >= MOTION_IDLE_WAKE_THRESHOLD_MG) ||
		    (idle_smooth_mg >= MOTION_IDLE_WAKE_SMOOTH_MG)) {
			g_motion.last_motion_wake_ms = now_ms;
			g_motion.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
			(void)app_event_publish_with_timestamp(APP_EVENT_MOTION_WAKE, idle_motion_mg,
							       now_ms);
			motion_classifier_set_mode(MOTION_CLASSIFIER_MODE_ACTIVE_WINDOW,
						   g_motion.active_until_ms);
			return;
		}

		(void)k_work_reschedule(&g_motion.sample_work, K_MSEC(MOTION_IDLE_POLL_MS));
		return;
	}

	hw_step_delta = update_hw_step_counter(now_ms);

	linear_x = sample.accel_mg_x - g_motion.gravity_x;
	linear_y = sample.accel_mg_y - g_motion.gravity_y;
	linear_z = sample.accel_mg_z - g_motion.gravity_z;
	orientation_delta_mg = (uint16_t)(abs_i16(g_motion.gravity_x - previous_gravity_x) +
					  abs_i16(g_motion.gravity_y - previous_gravity_y) +
					  abs_i16(g_motion.gravity_z - previous_gravity_z));
	orientation_rate_mg = orientation_delta_mg;
	jerk_mg = (uint16_t)(abs_i16(linear_x - g_motion.last_linear_x) +
			     abs_i16(linear_y - g_motion.last_linear_y) +
			     abs_i16(linear_z - g_motion.last_linear_z));
	motion_mg = (uint16_t)(abs_i16(linear_x) + abs_i16(linear_y) + abs_i16(linear_z));
	smooth_motion_mg = (uint16_t)(abs_i16(sample.accel_mg_x - g_motion.last_sample.accel_mg_x) +
				      abs_i16(sample.accel_mg_y - g_motion.last_sample.accel_mg_y) +
				      abs_i16(sample.accel_mg_z - g_motion.last_sample.accel_mg_z));
	gyro_sum_mdps = sample.gyro_valid ?
		(uint32_t)(abs_i16(sample.gyro_mdps_x) + abs_i16(sample.gyro_mdps_y) +
			   abs_i16(sample.gyro_mdps_z)) :
		0U;

	memset(&feature_frame, 0, sizeof(feature_frame));
	feature_frame.motion_mg = motion_mg;
	feature_frame.smooth_motion_mg = smooth_motion_mg;
	feature_frame.jerk_mg = jerk_mg;
	feature_frame.orientation_delta_mg = orientation_delta_mg;
	feature_frame.orientation_rate_mg = orientation_rate_mg;
	feature_frame.gyro_sum_mdps = gyro_sum_mdps;
	feature_frame.timestamp_ms = now_ms;
	append_feature_frame(&feature_frame);
	summarize_motion_features(&feature_summary, now_ms);
	g_motion.stability_confidence = feature_summary.stability_confidence;
	g_motion.cadence_confidence = feature_summary.cadence_confidence;
	g_motion.chaos_confidence = feature_summary.chaos_confidence;

	rough_motion = (motion_mg >= 900U) ||
		      (gyro_sum_mdps >= 60000U) ||
		      (feature_summary.chaos_confidence >= 72U);
	publish_motion_reaction(motion_mg,
			       gyro_sum_mdps,
			       g_motion.in_hand && (g_motion.in_hand_confidence >= 60U) &&
				       (feature_summary.stability_confidence >= 45U) &&
				       (feature_summary.chaos_confidence <= 45U),
			       now_ms);

	if (!motion_sensor_get_capabilities()->backend_has_hw_step_counter) {
		step_detected = detect_software_step(linear_x, linear_y, linear_z, rough_motion, now_ms);
	}
	update_walking_confidence(step_detected || (hw_step_delta > 0U), &feature_summary, now_ms);

	memset(&detector_in, 0, sizeof(detector_in));
	detector_in.gravity_x = g_motion.gravity_x;
	detector_in.gravity_y = g_motion.gravity_y;
	detector_in.gravity_z = g_motion.gravity_z;
	detector_in.motion_mg = motion_mg;
	detector_in.smooth_motion_mg = smooth_motion_mg;
	detector_in.jerk_mg = jerk_mg;
	detector_in.orientation_delta_mg = orientation_delta_mg;
	detector_in.orientation_rate_mg = orientation_rate_mg;
	detector_in.gyro_sum_mdps = gyro_sum_mdps;
	detector_in.walking_confidence = g_motion.walking_confidence;
	detector_in.cadence_confidence = feature_summary.cadence_confidence;
	detector_in.stability_confidence = feature_summary.stability_confidence;
	detector_in.chaos_confidence = feature_summary.chaos_confidence;
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
	update_look_reference(&detector_out, &feature_summary);
	update_look_target(&detector_out, &feature_summary, now_ms);
	publish_carry_state(now_ms, detector_out.pickup_candidate || detector_out.picked_up ||
				       detector_out.in_hand_enter || detector_out.in_hand_exit);
	publish_look_target(now_ms, false);

	g_motion.last_linear_x = linear_x;
	g_motion.last_linear_y = linear_y;
	g_motion.last_linear_z = linear_z;
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

	if ((now_ms - g_motion.last_motion_wake_ms) >= 750LL) {
		(void)app_event_publish_with_timestamp(APP_EVENT_MOTION_WAKE, (int32_t)events, now_ms);
	}
	g_motion.last_motion_wake_ms = now_ms;
	g_motion.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
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
	if (motion_classifier_uses_idle_polling()) {
		LOG_INF("Motion classifier using polling fallback until IMU interrupt is wired");
	}
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
		reset_look_reference();
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
		g_motion.suppress_look_until_ms = MAX(g_motion.suppress_look_until_ms,
						      event->timestamp_ms + 1200LL);
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
		if (!g_motion.in_hand) {
			reset_look_reference();
		}
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
		reset_look_reference();
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
