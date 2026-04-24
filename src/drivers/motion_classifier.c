/*
 * Kerfur IMU classifier.
 *
 * The public surface stays event-based. Internally the classifier is a small
 * pipeline:
 * raw sample -> dt-aware gravity -> linear acceleration -> feature frame ->
 * short/long windows -> walking, carry, shake and gaze decisions.
 */

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

#define DT_MIN_MS                       10
#define DT_MAX_MS                       180
#define DT_RESET_GAP_MS                 500LL

#define IDLE_POLL_MS                    800
#define IDLE_WAKE_LINEAR_MG             230U
#define IDLE_WAKE_DELTA_MG              170U

#define FEATURE_HISTORY_FRAMES          64U
#define FEATURE_SHORT_WINDOW_MS         650LL
#define FEATURE_LONG_WINDOW_MS          2200LL

#define GRAVITY_MIN_MAG_MG              820U
#define GRAVITY_MAX_MAG_MG              1180U
#define GRAVITY_CALM_TAU_MS             220U
#define GRAVITY_MOVING_TAU_MS           900U
#define GRAVITY_ROUGH_TAU_MS            1800U

#define STEP_PEAK_MG                    260U
#define STEP_MIN_INTERVAL_MS            300LL
#define STEP_MAX_INTERVAL_MS            900LL
#define STEP_RECENT_WINDOW_MS           2400LL

#define SURFACE_STILL_SCORE             78U
#define ACTIVE_MOTION_MG                55U

#define GAZE_DEADBAND                   4
#define GAZE_IDLE_RAW_THRESHOLD         7
#define GAZE_SLEW_UNITS_PER_S           280
#define GAZE_LOW_BATT_SLEW_UNITS_PER_S  150
#define GAZE_DECAY_UNITS_PER_S          55
#define GAZE_HOLD_AFTER_STOP_MS         2600LL
#define GAZE_HOLD_AFTER_LOST_MS         1200LL
#define GAZE_HOLD_CONFIDENCE            38U
#define GAZE_DECAY_CONFIDENCE           24U
#define LOOK_SUPPRESS_AFTER_ROUGH_MS    700LL
#define LOOK_SUPPRESS_AFTER_WALK_MS     500LL

#define SHAKE_COOLDOWN_LIGHT_MS         800LL
#define SHAKE_COOLDOWN_PLAY_MS          900LL
#define SHAKE_COOLDOWN_ROUGH_MS         1000LL
#define SHAKE_COOLDOWN_IMPACT_MS        1200LL

#define WALK_CONF_Q8_MAX                (100 * 256)

struct imu_processed_frame {
	int64_t now_ms;
	int32_t dt_ms;

	int16_t accel_x_mg;
	int16_t accel_y_mg;
	int16_t accel_z_mg;

	int16_t gravity_x_mg;
	int16_t gravity_y_mg;
	int16_t gravity_z_mg;

	int16_t linear_x_mg;
	int16_t linear_y_mg;
	int16_t linear_z_mg;

	uint16_t accel_magnitude_mg;
	uint16_t linear_motion_mg;
	uint16_t smooth_linear_motion_mg;
	uint16_t jerk_mg_per_s;
	uint16_t orientation_delta_mg;
	uint16_t orientation_rate_mg_per_s;
	uint32_t gyro_sum_mdps;
	uint32_t smooth_gyro_sum_mdps;

	uint8_t stability_score;
	uint8_t chaos_score;
	uint8_t cadence_score;
	uint8_t surface_still_score;
	uint8_t hand_motion_score;
	uint8_t rotation_score;

	bool gravity_valid;
	bool gyro_valid;
};

struct imu_window_summary {
	uint8_t count;
	uint16_t span_ms;

	uint16_t avg_accel_magnitude_mg;
	uint16_t avg_linear_motion_mg;
	uint16_t peak_linear_motion_mg;
	uint16_t avg_smooth_linear_motion_mg;
	uint16_t avg_jerk_mg_per_s;
	uint16_t peak_jerk_mg_per_s;
	uint16_t avg_orientation_delta_mg;
	uint16_t avg_orientation_rate_mg_per_s;
	uint16_t peak_orientation_rate_mg_per_s;
	uint32_t avg_gyro_sum_mdps;
	uint32_t peak_gyro_sum_mdps;

	uint8_t stability_score;
	uint8_t chaos_score;
	uint8_t cadence_score;
	uint8_t surface_still_score;
	uint8_t hand_motion_score;
	uint8_t rotation_score;
};

enum motion_classifier_mode {
	MODE_OFF = 0,
	MODE_IDLE,
	MODE_ACTIVE_WINDOW,
	MODE_WALK_MAINTAIN,
	MODE_IN_HAND_TRACK,
};

struct motion_classifier_state {
	struct k_work_delayable sample_work;
	struct in_hand_detector in_hand_det;
	enum motion_classifier_mode mode;
	struct motion_sensor_sample last_sample;

	bool enabled;
	bool initialized;
	bool debug_logging;
	bool battery_low;
	bool battery_critical;
	bool charging;
	bool battery_percent_known;
	bool gravity_valid;
	bool have_last_sample;
	bool have_prev_linear;
	bool hw_counter_valid;
	bool pending_steps_from_hw;
	bool over_step_threshold;
	bool walking_active;
	bool in_hand;
	bool look_reference_valid;

	int8_t battery_percent;

	int16_t gravity_x;
	int16_t gravity_y;
	int16_t gravity_z;
	int16_t prev_linear_x;
	int16_t prev_linear_y;
	int16_t prev_linear_z;

	uint16_t smooth_linear_motion_mg;
	uint32_t smooth_gyro_sum_mdps;

	int16_t look_ref_x;
	int16_t look_ref_y;
	int16_t look_ref_z;
	int16_t look_target_x;
	int16_t look_target_y;
	int16_t look_raw_prev_x;
	int16_t look_raw_prev_y;

	uint8_t walking_confidence;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t look_confidence;
	uint8_t stability_confidence;
	uint8_t chaos_confidence;
	uint8_t cadence_confidence;
	uint8_t surface_still_confidence;
	uint8_t hand_motion_confidence;
	uint8_t rotation_confidence;

	int32_t walking_confidence_q8;
	uint16_t last_hw_step_counter;
	uint32_t pending_steps;
	uint8_t recent_step_count;
	uint8_t recent_step_index;
	int64_t recent_step_ms[8];
	int64_t last_peak_ms;

	struct imu_processed_frame frames[FEATURE_HISTORY_FRAMES];
	uint8_t frame_count;
	uint8_t frame_index;

	int64_t active_until_ms;
	int64_t last_motion_wake_ms;
	int64_t suppress_look_until_ms;
	int64_t look_idle_since_ms;
	int64_t last_shake_event_ms;
	int64_t last_active_motion_ms;
	int64_t last_carry_publish_ms;
	int64_t last_look_publish_ms;
	int64_t last_debug_log_ms;
	int64_t last_pickup_ms;
	int64_t last_in_hand_ms;
	int64_t last_motion_sample_ms;
	int64_t last_still_ms;
	int64_t last_mode_change_ms;

	bool pub_in_hand;
	uint8_t pub_pickup_conf;
	uint8_t pub_in_hand_conf;
	uint8_t pub_walk_conf;
	int16_t pub_look_x;
	int16_t pub_look_y;
	uint8_t pub_look_conf;
};

static struct motion_classifier_state g_mc;

static uint8_t clamp_u8(int value)
{
	return (uint8_t)CLAMP(value, 0, 100);
}

static uint16_t clamp_u16_i32(int32_t value)
{
	return (uint16_t)CLAMP(value, 0, UINT16_MAX);
}

static int16_t clamp_look(int value)
{
	return (int16_t)CLAMP(value, -100, 100);
}

static int abs_i32(int value)
{
	return (value < 0) ? -value : value;
}

static int64_t max64(int64_t a, int64_t b)
{
	return (a > b) ? a : b;
}

static uint32_t isqrt_u32(uint32_t value)
{
	uint32_t bit = 1UL << 30;
	uint32_t result = 0;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}

	return result;
}

static uint16_t vec_mag_mg(int16_t x, int16_t y, int16_t z)
{
	uint32_t xx = (uint32_t)((int32_t)x * (int32_t)x);
	uint32_t yy = (uint32_t)((int32_t)y * (int32_t)y);
	uint32_t zz = (uint32_t)((int32_t)z * (int32_t)z);

	return clamp_u16_i32((int32_t)isqrt_u32(xx + yy + zz));
}

static uint16_t vec_distance_mg(int16_t ax, int16_t ay, int16_t az,
				int16_t bx, int16_t by, int16_t bz)
{
	return vec_mag_mg((int16_t)(ax - bx), (int16_t)(ay - by),
			  (int16_t)(az - bz));
}

static int sample_period_ms(void)
{
	int hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;

	return (hz > 0) ? MAX(DT_MIN_MS, 1000 / hz) : 40;
}

static int32_t sample_dt_ms(const struct motion_sensor_sample *sample,
			    bool *reset_history)
{
	int64_t dt;

	*reset_history = false;
	if (!g_mc.have_last_sample || g_mc.last_sample.timestamp_ms <= 0LL ||
	    sample->timestamp_ms <= g_mc.last_sample.timestamp_ms) {
		return sample_period_ms();
	}

	dt = sample->timestamp_ms - g_mc.last_sample.timestamp_ms;
	if (dt > DT_RESET_GAP_MS) {
		*reset_history = true;
	}

	return (int32_t)CLAMP(dt, DT_MIN_MS, DT_MAX_MS);
}

static uint16_t lp_denominator(uint16_t tau_ms, int32_t dt_ms)
{
	if (dt_ms <= 0) {
		return tau_ms;
	}
	if ((uint16_t)dt_ms >= tau_ms) {
		return 1U;
	}

	return (uint16_t)MAX(1, (tau_ms + (uint16_t)dt_ms / 2U) / (uint16_t)dt_ms);
}

static int16_t lp_i16_dt(int16_t current, int16_t target,
			 uint16_t tau_ms, int32_t dt_ms)
{
	int32_t delta = (int32_t)target - (int32_t)current;
	uint16_t denom = lp_denominator(tau_ms, dt_ms);

	return (int16_t)(current + (int16_t)(delta / denom));
}

static uint16_t lp_u16_dt(uint16_t current, uint16_t target,
			  uint16_t tau_ms, int32_t dt_ms)
{
	int32_t delta = (int32_t)target - (int32_t)current;
	uint16_t denom = lp_denominator(tau_ms, dt_ms);

	return (uint16_t)((int32_t)current + (delta / denom));
}

static uint32_t lp_u32_dt(uint32_t current, uint32_t target,
			  uint16_t tau_ms, int32_t dt_ms)
{
	int64_t delta = (int64_t)target - (int64_t)current;
	uint16_t denom = lp_denominator(tau_ms, dt_ms);

	return (uint32_t)((int64_t)current + (delta / denom));
}

static uint16_t rate_u16(uint16_t delta, int32_t dt_ms)
{
	if (dt_ms <= 0) {
		return 0U;
	}

	return clamp_u16_i32((int32_t)(((uint32_t)delta * 1000U) / (uint32_t)dt_ms));
}

static uint8_t score_more_equal_u16(uint16_t value, uint16_t full_at)
{
	if (value >= full_at) {
		return 100U;
	}

	return (uint8_t)(((uint32_t)value * 100U) / full_at);
}

static uint8_t score_band_u16(uint16_t value, uint16_t low, uint16_t ideal,
			      uint16_t high)
{
	if (value < low || value > high) {
		return 0U;
	}
	if (value == ideal) {
		return 100U;
	}
	if (value < ideal) {
		return (uint8_t)(((uint32_t)(value - low) * 100U) /
				 MAX(1U, (uint32_t)(ideal - low)));
	}

	return (uint8_t)(100U - (((uint32_t)(value - ideal) * 100U) /
				 MAX(1U, (uint32_t)(high - ideal))));
}

static void set_walking_confidence(uint8_t confidence)
{
	g_mc.walking_confidence = clamp_u8(confidence);
	g_mc.walking_confidence_q8 = (int32_t)g_mc.walking_confidence * 256;
}

static void adjust_walking_confidence(int rate_per_s, int32_t dt_ms)
{
	int64_t delta;

	dt_ms = CLAMP(dt_ms, DT_MIN_MS, DT_MAX_MS);
	delta = (int64_t)rate_per_s * 256LL * (int64_t)dt_ms;
	g_mc.walking_confidence_q8 = (int32_t)CLAMP(
		(int64_t)g_mc.walking_confidence_q8 + (delta / 1000LL),
		0LL, (int64_t)WALK_CONF_Q8_MAX);
	g_mc.walking_confidence = (uint8_t)((g_mc.walking_confidence_q8 + 128) / 256);
}

static bool accel_magnitude_plausible(uint16_t magnitude_mg)
{
	return magnitude_mg >= GRAVITY_MIN_MAG_MG &&
	       magnitude_mg <= GRAVITY_MAX_MAG_MG;
}

static void update_gravity(const struct motion_sensor_sample *sample,
			   uint16_t accel_mag_mg,
			   uint16_t accel_delta_mg,
			   uint32_t gyro_sum_mdps,
			   int32_t dt_ms)
{
	bool plausible = accel_magnitude_plausible(accel_mag_mg);
	bool calm;
	uint16_t tau_ms;

	if (!g_mc.gravity_valid) {
		if (plausible) {
			g_mc.gravity_x = sample->accel_mg_x;
			g_mc.gravity_y = sample->accel_mg_y;
			g_mc.gravity_z = sample->accel_mg_z;
			g_mc.gravity_valid = true;
		}
		return;
	}

	if (!plausible) {
		return;
	}

	calm = accel_delta_mg <= 45U && gyro_sum_mdps <= 6000U;
	if (calm) {
		tau_ms = GRAVITY_CALM_TAU_MS;
	} else if (accel_delta_mg >= 220U || gyro_sum_mdps >= 35000U) {
		tau_ms = GRAVITY_ROUGH_TAU_MS;
	} else {
		tau_ms = GRAVITY_MOVING_TAU_MS;
	}

	g_mc.gravity_x = lp_i16_dt(g_mc.gravity_x, sample->accel_mg_x, tau_ms, dt_ms);
	g_mc.gravity_y = lp_i16_dt(g_mc.gravity_y, sample->accel_mg_y, tau_ms, dt_ms);
	g_mc.gravity_z = lp_i16_dt(g_mc.gravity_z, sample->accel_mg_z, tau_ms, dt_ms);

	if (!accel_magnitude_plausible(vec_mag_mg(g_mc.gravity_x, g_mc.gravity_y,
						  g_mc.gravity_z))) {
		g_mc.gravity_valid = false;
	}
}

static uint8_t compute_stability_score(const struct imu_processed_frame *f)
{
	int score = 100;
	int accel_error = abs_i32((int)f->accel_magnitude_mg - 1000);

	score -= MIN(45, ((int)f->smooth_linear_motion_mg * 45) / 180);
	score -= MIN(35, ((int)f->jerk_mg_per_s * 35) / 3200);
	score -= MIN(30, ((int)f->smooth_gyro_sum_mdps * 30) / 18000);
	score -= MIN(25, ((int)f->orientation_rate_mg_per_s * 25) / 650);
	if (accel_error > 120) {
		score -= MIN(25, (accel_error - 120) / 8);
	}

	return clamp_u8(score);
}

static uint8_t compute_surface_still_score(const struct imu_processed_frame *f)
{
	int score = 100;
	int accel_error = abs_i32((int)f->accel_magnitude_mg - 1000);

	score -= MIN(55, ((int)f->smooth_linear_motion_mg * 55) / 75);
	score -= MIN(35, ((int)f->jerk_mg_per_s * 35) / 1300);
	score -= MIN(35, ((int)f->smooth_gyro_sum_mdps * 35) / 6500);
	score -= MIN(30, ((int)f->orientation_rate_mg_per_s * 30) / 180);
	if (accel_error > 75) {
		score -= MIN(35, (accel_error - 75) / 4);
	}

	return clamp_u8(score);
}

static uint8_t compute_chaos_score(const struct imu_processed_frame *f)
{
	int score = 0;
	int accel_error = abs_i32((int)f->accel_magnitude_mg - 1000);

	if (f->linear_motion_mg > 360U) {
		score += ((int)f->linear_motion_mg - 360) / 7;
	}
	if (f->jerk_mg_per_s > 2800U) {
		score += ((int)f->jerk_mg_per_s - 2800) / 80;
	}
	if (f->gyro_sum_mdps > 26000U) {
		score += ((int)f->gyro_sum_mdps - 26000) / 900;
	}
	if (accel_error > 180) {
		score += (accel_error - 180) / 6;
	}

	return clamp_u8(score);
}

static uint8_t compute_rotation_score(const struct imu_processed_frame *f)
{
	int score = 0;

	score += MIN(55, ((int)f->orientation_rate_mg_per_s * 55) / 520);
	score += MIN(55, ((int)f->gyro_sum_mdps * 55) / 36000);
	if (f->linear_motion_mg > 420U) {
		score -= MIN(35, ((int)f->linear_motion_mg - 420) / 10);
	}

	return clamp_u8(score);
}

static uint8_t compute_hand_motion_score(const struct imu_processed_frame *f)
{
	int score = 0;

	score += (int)score_band_u16(f->linear_motion_mg, 8U, 70U, 360U) / 2;
	score += (int)score_band_u16(f->smooth_linear_motion_mg, 6U, 45U, 220U) / 3;
	score += (int)score_band_u16(f->orientation_rate_mg_per_s, 25U, 220U, 900U) / 3;
	score += (int)score_band_u16((uint16_t)MIN(f->gyro_sum_mdps, 60000U),
				     1500U, 14000U, 52000U) / 4;
	score += (int)f->stability_score / 6;
	score -= (int)f->chaos_score / 2;

	return clamp_u8(score);
}

static bool build_frame(const struct motion_sensor_sample *sample,
			struct imu_processed_frame *frame)
{
	bool reset_history;
	int32_t dt_ms;
	uint16_t accel_mag;
	uint16_t accel_delta = 0U;
	uint32_t gyro_sum = 0U;
	int16_t prev_gravity_x;
	int16_t prev_gravity_y;
	int16_t prev_gravity_z;
	bool had_gravity;
	int16_t linear_x;
	int16_t linear_y;
	int16_t linear_z;
	uint16_t linear_delta = 0U;
	uint16_t orientation_delta;

	memset(frame, 0, sizeof(*frame));

	dt_ms = sample_dt_ms(sample, &reset_history);
	accel_mag = vec_mag_mg(sample->accel_mg_x, sample->accel_mg_y,
			       sample->accel_mg_z);
	if (g_mc.have_last_sample && !reset_history) {
		accel_delta = vec_distance_mg(sample->accel_mg_x, sample->accel_mg_y,
					      sample->accel_mg_z,
					      g_mc.last_sample.accel_mg_x,
					      g_mc.last_sample.accel_mg_y,
					      g_mc.last_sample.accel_mg_z);
	}
	if (sample->gyro_valid) {
		gyro_sum = (uint32_t)abs_i32(sample->gyro_mdps_x) +
			   (uint32_t)abs_i32(sample->gyro_mdps_y) +
			   (uint32_t)abs_i32(sample->gyro_mdps_z);
	}

	prev_gravity_x = g_mc.gravity_x;
	prev_gravity_y = g_mc.gravity_y;
	prev_gravity_z = g_mc.gravity_z;
	had_gravity = g_mc.gravity_valid;
	update_gravity(sample, accel_mag, accel_delta, gyro_sum, dt_ms);
	if (!g_mc.gravity_valid) {
		return false;
	}

	linear_x = sample->accel_mg_x - g_mc.gravity_x;
	linear_y = sample->accel_mg_y - g_mc.gravity_y;
	linear_z = sample->accel_mg_z - g_mc.gravity_z;
	if (g_mc.have_prev_linear && !reset_history) {
		linear_delta = vec_distance_mg(linear_x, linear_y, linear_z,
					       g_mc.prev_linear_x,
					       g_mc.prev_linear_y,
					       g_mc.prev_linear_z);
	}

	if (had_gravity && !reset_history) {
		orientation_delta = vec_distance_mg(g_mc.gravity_x, g_mc.gravity_y,
						    g_mc.gravity_z, prev_gravity_x,
						    prev_gravity_y, prev_gravity_z);
	} else {
		orientation_delta = 0U;
	}

	g_mc.smooth_linear_motion_mg = lp_u16_dt(g_mc.smooth_linear_motion_mg,
						 vec_mag_mg(linear_x, linear_y,
							    linear_z),
						 180U, dt_ms);
	g_mc.smooth_gyro_sum_mdps = lp_u32_dt(g_mc.smooth_gyro_sum_mdps, gyro_sum,
					      180U, dt_ms);

	frame->now_ms = sample->timestamp_ms;
	frame->dt_ms = dt_ms;
	frame->accel_x_mg = sample->accel_mg_x;
	frame->accel_y_mg = sample->accel_mg_y;
	frame->accel_z_mg = sample->accel_mg_z;
	frame->gravity_x_mg = g_mc.gravity_x;
	frame->gravity_y_mg = g_mc.gravity_y;
	frame->gravity_z_mg = g_mc.gravity_z;
	frame->linear_x_mg = linear_x;
	frame->linear_y_mg = linear_y;
	frame->linear_z_mg = linear_z;
	frame->accel_magnitude_mg = accel_mag;
	frame->linear_motion_mg = vec_mag_mg(linear_x, linear_y, linear_z);
	frame->smooth_linear_motion_mg = g_mc.smooth_linear_motion_mg;
	frame->jerk_mg_per_s = rate_u16(linear_delta, dt_ms);
	frame->orientation_delta_mg = orientation_delta;
	frame->orientation_rate_mg_per_s = rate_u16(orientation_delta, dt_ms);
	frame->gyro_sum_mdps = gyro_sum;
	frame->smooth_gyro_sum_mdps = g_mc.smooth_gyro_sum_mdps;
	frame->gravity_valid = g_mc.gravity_valid;
	frame->gyro_valid = sample->gyro_valid;

	frame->stability_score = compute_stability_score(frame);
	frame->surface_still_score = compute_surface_still_score(frame);
	frame->chaos_score = compute_chaos_score(frame);
	frame->rotation_score = compute_rotation_score(frame);
	frame->hand_motion_score = compute_hand_motion_score(frame);
	frame->cadence_score = g_mc.cadence_confidence;

	if (reset_history) {
		g_mc.frame_count = 0U;
		g_mc.frame_index = 0U;
	}

	return true;
}

static void append_frame(const struct imu_processed_frame *frame)
{
	g_mc.frames[g_mc.frame_index] = *frame;
	g_mc.frame_index = (g_mc.frame_index + 1U) % FEATURE_HISTORY_FRAMES;
	if (g_mc.frame_count < FEATURE_HISTORY_FRAMES) {
		g_mc.frame_count++;
	}
}

static void summarise_window(struct imu_window_summary *out, int64_t now_ms,
			     int64_t window_ms)
{
	uint32_t sum_accel = 0U;
	uint32_t sum_linear = 0U;
	uint32_t sum_smooth = 0U;
	uint32_t sum_jerk = 0U;
	uint32_t sum_orient_delta = 0U;
	uint32_t sum_orient_rate = 0U;
	uint64_t sum_gyro = 0U;
	uint32_t sum_stability = 0U;
	uint32_t sum_chaos = 0U;
	uint32_t sum_cadence = 0U;
	uint32_t sum_surface = 0U;
	uint32_t sum_hand = 0U;
	uint32_t sum_rotation = 0U;
	uint8_t count = 0U;
	int64_t oldest_ms = now_ms;

	memset(out, 0, sizeof(*out));

	for (uint8_t i = 0U; i < g_mc.frame_count; i++) {
		const struct imu_processed_frame *f = &g_mc.frames[i];

		if (f->now_ms <= 0LL || (now_ms - f->now_ms) > window_ms) {
			continue;
		}
		count++;
		oldest_ms = MIN(oldest_ms, f->now_ms);

		sum_accel += f->accel_magnitude_mg;
		sum_linear += f->linear_motion_mg;
		sum_smooth += f->smooth_linear_motion_mg;
		sum_jerk += f->jerk_mg_per_s;
		sum_orient_delta += f->orientation_delta_mg;
		sum_orient_rate += f->orientation_rate_mg_per_s;
		sum_gyro += f->gyro_sum_mdps;
		sum_stability += f->stability_score;
		sum_chaos += f->chaos_score;
		sum_cadence += f->cadence_score;
		sum_surface += f->surface_still_score;
		sum_hand += f->hand_motion_score;
		sum_rotation += f->rotation_score;

		out->peak_linear_motion_mg = MAX(out->peak_linear_motion_mg,
						 f->linear_motion_mg);
		out->peak_jerk_mg_per_s = MAX(out->peak_jerk_mg_per_s,
					      f->jerk_mg_per_s);
		out->peak_orientation_rate_mg_per_s =
			MAX(out->peak_orientation_rate_mg_per_s,
			    f->orientation_rate_mg_per_s);
		out->peak_gyro_sum_mdps = MAX(out->peak_gyro_sum_mdps,
					      f->gyro_sum_mdps);
	}

	if (count == 0U) {
		return;
	}

	out->count = count;
	out->span_ms = clamp_u16_i32((int32_t)(now_ms - oldest_ms));
	out->avg_accel_magnitude_mg = (uint16_t)(sum_accel / count);
	out->avg_linear_motion_mg = (uint16_t)(sum_linear / count);
	out->avg_smooth_linear_motion_mg = (uint16_t)(sum_smooth / count);
	out->avg_jerk_mg_per_s = (uint16_t)(sum_jerk / count);
	out->avg_orientation_delta_mg = (uint16_t)(sum_orient_delta / count);
	out->avg_orientation_rate_mg_per_s = (uint16_t)(sum_orient_rate / count);
	out->avg_gyro_sum_mdps = (uint32_t)(sum_gyro / count);
	out->stability_score = (uint8_t)(sum_stability / count);
	out->chaos_score = (uint8_t)MAX(sum_chaos / count,
				       score_more_equal_u16(out->peak_jerk_mg_per_s,
							    9000U) / 2U);
	out->cadence_score = (uint8_t)(sum_cadence / count);
	out->surface_still_score = (uint8_t)(sum_surface / count);
	out->hand_motion_score = (uint8_t)(sum_hand / count);
	out->rotation_score = (uint8_t)(sum_rotation / count);
}

static void collect_recent_steps(int64_t now_ms, int64_t *steps, uint8_t *count)
{
	*count = 0U;
	for (uint8_t i = 0U; i < ARRAY_SIZE(g_mc.recent_step_ms); i++) {
		int64_t t = g_mc.recent_step_ms[i];

		if (t <= 0LL || (now_ms - t) > STEP_RECENT_WINDOW_MS) {
			continue;
		}
		steps[*count] = t;
		(*count)++;
	}

	for (uint8_t i = 1U; i < *count; i++) {
		int64_t key = steps[i];
		int j = (int)i - 1;

		while (j >= 0 && steps[j] > key) {
			steps[j + 1] = steps[j];
			j--;
		}
		steps[j + 1] = key;
	}
}

static void note_step(int64_t now_ms)
{
	g_mc.recent_step_ms[g_mc.recent_step_index] = now_ms;
	g_mc.recent_step_index =
		(g_mc.recent_step_index + 1U) % ARRAY_SIZE(g_mc.recent_step_ms);
	if (g_mc.recent_step_count < ARRAY_SIZE(g_mc.recent_step_ms)) {
		g_mc.recent_step_count++;
	}
	g_mc.last_peak_ms = now_ms;
}

static uint8_t compute_cadence_confidence(int64_t now_ms)
{
	int64_t steps[ARRAY_SIZE(g_mc.recent_step_ms)];
	int64_t intervals[ARRAY_SIZE(g_mc.recent_step_ms) - 1U];
	uint8_t step_count;
	uint8_t interval_count = 0U;
	int64_t sum = 0LL;
	int64_t avg;
	int64_t err_sum = 0LL;

	collect_recent_steps(now_ms, steps, &step_count);
	if (step_count < 3U) {
		return 0U;
	}

	for (uint8_t i = 1U; i < step_count; i++) {
		int64_t dt = steps[i] - steps[i - 1U];

		if (dt >= 250LL && dt <= 1100LL) {
			intervals[interval_count++] = dt;
			sum += dt;
		}
	}
	if (interval_count < 2U) {
		return 0U;
	}

	avg = sum / interval_count;
	if (avg < 300LL || avg > 950LL) {
		return 0U;
	}

	for (uint8_t i = 0U; i < interval_count; i++) {
		int64_t err = intervals[i] - avg;

		err_sum += (err < 0LL) ? -err : err;
	}

	return clamp_u8(28 + (int)step_count * 12 -
			(int)(err_sum / MAX(1U, interval_count)) / 5);
}

static void flush_steps(int64_t now_ms)
{
	if (g_mc.pending_steps == 0U) {
		return;
	}

	(void)app_event_publish_step_batch_with_timestamp(
		(int32_t)g_mc.pending_steps, g_mc.last_hw_step_counter,
		g_mc.walking_confidence, g_mc.pending_steps_from_hw, now_ms);
	g_mc.pending_steps = 0U;
	g_mc.pending_steps_from_hw = false;
}

static uint16_t read_hw_steps(int64_t now_ms)
{
	uint16_t hw = 0U;
	uint16_t delta;

	if (motion_sensor_read_hw_step_counter(&hw) != 0) {
		return 0U;
	}
	if (!g_mc.hw_counter_valid) {
		g_mc.last_hw_step_counter = hw;
		g_mc.hw_counter_valid = true;
		return 0U;
	}
	if (hw == g_mc.last_hw_step_counter) {
		return 0U;
	}
	if (hw >= g_mc.last_hw_step_counter) {
		delta = hw - g_mc.last_hw_step_counter;
	} else {
		delta = (uint16_t)(UINT16_MAX - g_mc.last_hw_step_counter + hw + 1U);
	}

	g_mc.last_hw_step_counter = hw;
	if (delta > 128U) {
		return 0U;
	}

	g_mc.pending_steps += delta;
	g_mc.pending_steps_from_hw = true;
	for (uint16_t i = 0U; i < delta; i++) {
		int64_t step_ms = now_ms - ((int64_t)(delta - 1U - i) * 500LL);

		note_step(MAX(1LL, step_ms));
	}
	if (g_mc.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
		flush_steps(now_ms);
	}

	return delta;
}

static bool detect_sw_step(const struct imu_processed_frame *frame,
			   const struct imu_window_summary *short_win,
			   bool rough)
{
	bool over;
	int64_t dt;

	if (rough || short_win->chaos_score >= 72U ||
	    frame->rotation_score >= 82U ||
	    frame->linear_motion_mg < STEP_PEAK_MG) {
		g_mc.over_step_threshold = false;
		return false;
	}

	over = frame->linear_motion_mg >= STEP_PEAK_MG &&
	       frame->jerk_mg_per_s >= 1000U &&
	       frame->jerk_mg_per_s <= 9000U;
	dt = frame->now_ms - g_mc.last_peak_ms;

	if (!g_mc.over_step_threshold && over &&
	    (g_mc.last_peak_ms == 0LL ||
	     (dt >= STEP_MIN_INTERVAL_MS && dt <= STEP_MAX_INTERVAL_MS))) {
		note_step(frame->now_ms);
		g_mc.pending_steps++;
		if (g_mc.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
			flush_steps(frame->now_ms);
		}
		g_mc.over_step_threshold = true;
		return true;
	}

	g_mc.over_step_threshold = over;
	return false;
}

static void update_walking(bool step, const struct imu_processed_frame *frame,
			   const struct imu_window_summary *short_win)
{
	int rate;
	bool cadence = g_mc.cadence_confidence >= 45U;
	bool walking_motion =
		short_win->avg_linear_motion_mg >= 65U &&
		short_win->avg_linear_motion_mg <= 620U &&
		short_win->chaos_score <= 58U &&
		short_win->rotation_score <= 78U;

	if (step) {
		adjust_walking_confidence(0, frame->dt_ms);
		g_mc.walking_confidence_q8 =
			MIN(g_mc.walking_confidence_q8 + (9 * 256),
			    WALK_CONF_Q8_MAX);
		g_mc.walking_confidence =
			(uint8_t)((g_mc.walking_confidence_q8 + 128) / 256);
	}

	if (cadence && walking_motion) {
		rate = step ? 42 : 22;
	} else if (cadence && g_mc.walking_active) {
		rate = 6;
	} else if (short_win->chaos_score >= 72U || frame->rotation_score >= 88U) {
		rate = -45;
	} else if (g_mc.last_peak_ms > 0LL &&
		   (frame->now_ms - g_mc.last_peak_ms) > 1400LL) {
		rate = -34;
	} else {
		rate = -22;
	}

	adjust_walking_confidence(rate, frame->dt_ms);

	if (!g_mc.walking_active &&
	    g_mc.walking_confidence >= CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD) {
		g_mc.walking_active = true;
		g_mc.suppress_look_until_ms =
			max64(g_mc.suppress_look_until_ms,
			      frame->now_ms + LOOK_SUPPRESS_AFTER_WALK_MS);
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_START,
						       g_mc.walking_confidence,
						       frame->now_ms);
	} else if (g_mc.walking_active &&
		   g_mc.walking_confidence <= CONFIG_KERFUR_MOTION_WALK_STOP_THRESHOLD) {
		g_mc.walking_active = false;
		flush_steps(frame->now_ms);
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_STOP,
						       g_mc.walking_confidence,
						       frame->now_ms);
	}
}

static bool is_rough_motion(const struct imu_processed_frame *frame,
			    const struct imu_window_summary *short_win)
{
	return frame->linear_motion_mg >= 1200U ||
	       frame->jerk_mg_per_s >= 12000U ||
	       frame->gyro_sum_mdps >= 90000U ||
	       short_win->chaos_score >= 82U;
}

static void check_shake(const struct imu_processed_frame *frame,
			const struct imu_window_summary *short_win,
			bool in_hand, uint8_t in_hand_conf)
{
	enum app_event_type type = APP_EVENT_COUNT;
	int64_t cooldown = 0LL;
	uint16_t linear_peak = MAX(frame->linear_motion_mg,
				   short_win->peak_linear_motion_mg);
	uint16_t jerk_peak = MAX(frame->jerk_mg_per_s,
				 short_win->peak_jerk_mg_per_s);
	uint32_t gyro_peak = MAX(frame->gyro_sum_mdps,
				 short_win->peak_gyro_sum_mdps);

	if (linear_peak >= 1900U || jerk_peak >= 18000U || gyro_peak >= 125000U) {
		type = APP_EVENT_IMPACT;
		cooldown = SHAKE_COOLDOWN_IMPACT_MS;
		g_mc.suppress_look_until_ms =
			max64(g_mc.suppress_look_until_ms,
			      frame->now_ms + LOOK_SUPPRESS_AFTER_ROUGH_MS);
	} else if ((linear_peak >= 1300U && jerk_peak >= 9000U) ||
		   gyro_peak >= 95000U) {
		type = APP_EVENT_SHAKE_ROUGH;
		cooldown = SHAKE_COOLDOWN_ROUGH_MS;
		g_mc.suppress_look_until_ms =
			max64(g_mc.suppress_look_until_ms,
			      frame->now_ms + LOOK_SUPPRESS_AFTER_ROUGH_MS);
	} else if ((linear_peak >= 900U && jerk_peak >= 6500U &&
		    short_win->chaos_score >= 48U) ||
		   gyro_peak >= 76000U) {
		type = APP_EVENT_SHAKE_PLAY;
		cooldown = SHAKE_COOLDOWN_PLAY_MS;
	} else if (linear_peak >= 680U && jerk_peak >= 5200U &&
		   short_win->chaos_score >= 40U) {
		type = APP_EVENT_SHAKE_LIGHT;
		cooldown = SHAKE_COOLDOWN_LIGHT_MS;
	}

	if (type == APP_EVENT_COUNT) {
		return;
	}
	if ((type == APP_EVENT_SHAKE_LIGHT || type == APP_EVENT_SHAKE_PLAY) &&
	    (in_hand || in_hand_conf >= 30U || g_mc.walking_active)) {
		return;
	}
	if ((frame->now_ms - g_mc.last_shake_event_ms) < cooldown) {
		return;
	}

	g_mc.last_shake_event_ms = frame->now_ms;
	(void)app_event_publish_with_timestamp(type, 0, frame->now_ms);
}

static void reset_look_reference(void)
{
	g_mc.look_reference_valid = false;
	g_mc.look_ref_x = 0;
	g_mc.look_ref_y = 0;
	g_mc.look_ref_z = 0;
	g_mc.look_raw_prev_x = 0;
	g_mc.look_raw_prev_y = 0;
}

static void capture_look_reference(int64_t now_ms)
{
	if (!g_mc.gravity_valid) {
		return;
	}

	g_mc.look_ref_x = g_mc.gravity_x;
	g_mc.look_ref_y = g_mc.gravity_y;
	g_mc.look_ref_z = g_mc.gravity_z;
	g_mc.look_reference_valid = true;
	g_mc.look_target_x = 0;
	g_mc.look_target_y = 0;
	g_mc.look_raw_prev_x = 0;
	g_mc.look_raw_prev_y = 0;
	g_mc.look_idle_since_ms = now_ms;
}

static int16_t move_towards_dt(int16_t current, int16_t target,
			       int units_per_s, int32_t dt_ms)
{
	int delta = target - current;
	int max_delta;

	if (delta == 0) {
		return current;
	}

	max_delta = MAX(1, (units_per_s * dt_ms) / 1000);
	if (delta > max_delta) {
		delta = max_delta;
	} else if (delta < -max_delta) {
		delta = -max_delta;
	}

	return (int16_t)(current + delta);
}

static int16_t apply_look_deadband(int16_t value)
{
	if (abs_i32(value) <= GAZE_DEADBAND) {
		return 0;
	}

	return (value > 0) ? (int16_t)(value - GAZE_DEADBAND) :
			     (int16_t)(value + GAZE_DEADBAND);
}

static void update_look_reference(const struct in_hand_detector_output *det,
				  int64_t now_ms)
{
	if (det->in_hand_enter || det->picked_up ||
	    (!g_mc.look_reference_valid && det->in_hand_confidence >= 28U)) {
		capture_look_reference(now_ms);
		return;
	}

	if (det->state == IN_HAND_DETECTOR_SURFACE_STILL && !det->in_hand) {
		if (g_mc.look_idle_since_ms == 0LL) {
			g_mc.look_idle_since_ms = now_ms;
		}
		if (g_mc.look_target_x == 0 && g_mc.look_target_y == 0) {
			reset_look_reference();
		}
	}
}

static void update_look_target(const struct in_hand_detector_output *det,
			       const struct imu_processed_frame *frame,
			       const struct imu_window_summary *short_win)
{
	int16_t raw_x;
	int16_t raw_y;
	int slew;
	bool tracking;
	bool decaying = false;

	if (frame->chaos_score >= 80U || frame->gyro_sum_mdps >= 80000U) {
		g_mc.suppress_look_until_ms =
			max64(g_mc.suppress_look_until_ms,
			      frame->now_ms + LOOK_SUPPRESS_AFTER_ROUGH_MS);
	}

	tracking = frame->now_ms >= g_mc.suppress_look_until_ms &&
		   !g_mc.walking_active &&
		   g_mc.look_reference_valid &&
		   frame->gravity_valid &&
		   (det->in_hand || det->look_confidence >= 18U);

	if (tracking) {
		raw_x = clamp_look((-(g_mc.gravity_x - g_mc.look_ref_x) * 100) /
				   CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);
		raw_y = clamp_look(((g_mc.gravity_y - g_mc.look_ref_y) * 100) /
				   CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);
		raw_x = apply_look_deadband(raw_x);
		raw_y = apply_look_deadband(raw_y);
	} else {
		raw_x = g_mc.look_target_x;
		raw_y = g_mc.look_target_y;
	}

	if (tracking &&
	    (abs_i32(raw_x - g_mc.look_raw_prev_x) >= GAZE_IDLE_RAW_THRESHOLD ||
	     abs_i32(raw_y - g_mc.look_raw_prev_y) >= GAZE_IDLE_RAW_THRESHOLD ||
	     short_win->rotation_score >= 35U)) {
		g_mc.look_idle_since_ms = frame->now_ms;
	}
	g_mc.look_raw_prev_x = raw_x;
	g_mc.look_raw_prev_y = raw_y;

	if (tracking) {
		if (g_mc.look_idle_since_ms > 0LL &&
		    (frame->now_ms - g_mc.look_idle_since_ms) >=
		    GAZE_HOLD_AFTER_STOP_MS) {
			decaying = true;
		}
	} else if (g_mc.look_target_x != 0 || g_mc.look_target_y != 0) {
		if (g_mc.look_idle_since_ms == 0LL) {
			g_mc.look_idle_since_ms = frame->now_ms;
		}
		if ((frame->now_ms - g_mc.look_idle_since_ms) >=
		    GAZE_HOLD_AFTER_LOST_MS) {
			decaying = true;
		}
	}

	if (decaying) {
		g_mc.look_target_x = move_towards_dt(g_mc.look_target_x, 0,
						     GAZE_DECAY_UNITS_PER_S,
						     frame->dt_ms);
		g_mc.look_target_y = move_towards_dt(g_mc.look_target_y, 0,
						     GAZE_DECAY_UNITS_PER_S,
						     frame->dt_ms);
	} else if (tracking) {
		slew = g_mc.battery_low ? GAZE_LOW_BATT_SLEW_UNITS_PER_S :
					  GAZE_SLEW_UNITS_PER_S;
		g_mc.look_target_x = move_towards_dt(g_mc.look_target_x, raw_x,
						     slew, frame->dt_ms);
		g_mc.look_target_y = move_towards_dt(g_mc.look_target_y, raw_y,
						     slew, frame->dt_ms);
	}

	if (tracking && !decaying) {
		g_mc.look_confidence = clamp_u8(
			(int)det->look_confidence -
			(int)short_win->chaos_score / 3 -
			(int)g_mc.cadence_confidence / 4);
	} else if (g_mc.look_target_x == 0 && g_mc.look_target_y == 0) {
		g_mc.look_confidence = 0U;
	} else {
		g_mc.look_confidence = decaying ? GAZE_DECAY_CONFIDENCE :
						  GAZE_HOLD_CONFIDENCE;
	}
}

static void publish_carry(int64_t now_ms, bool force)
{
	bool state_changed = g_mc.pub_in_hand != g_mc.in_hand;
	bool confidence_changed =
		abs_i32((int)g_mc.pub_pickup_conf - (int)g_mc.pickup_confidence) >= 4 ||
		abs_i32((int)g_mc.pub_in_hand_conf - (int)g_mc.in_hand_confidence) >= 4 ||
		abs_i32((int)g_mc.pub_walk_conf - (int)g_mc.walking_confidence) >= 5;

	if (!force && !state_changed && !confidence_changed) {
		return;
	}
	if (!force && (now_ms - g_mc.last_carry_publish_ms) < 250LL) {
		return;
	}

	(void)app_event_publish_carry_state_with_timestamp(
		g_mc.in_hand, g_mc.pickup_confidence,
		g_mc.in_hand_confidence, g_mc.walking_confidence, now_ms);

	g_mc.last_carry_publish_ms = now_ms;
	g_mc.pub_in_hand = g_mc.in_hand;
	g_mc.pub_pickup_conf = g_mc.pickup_confidence;
	g_mc.pub_in_hand_conf = g_mc.in_hand_confidence;
	g_mc.pub_walk_conf = g_mc.walking_confidence;
}

static void publish_look(int64_t now_ms, bool force)
{
	int min_ms = MAX(50, 1000 / CONFIG_KERFUR_MOTION_GAZE_RATE_HZ);
	bool changed =
		abs_i32(g_mc.look_target_x - g_mc.pub_look_x) >= 2 ||
		abs_i32(g_mc.look_target_y - g_mc.pub_look_y) >= 2 ||
		abs_i32((int)g_mc.look_confidence - (int)g_mc.pub_look_conf) >= 3 ||
		(g_mc.look_confidence == 0U && g_mc.pub_look_conf != 0U) ||
		(g_mc.look_target_x == 0 && g_mc.look_target_y == 0 &&
		 (g_mc.pub_look_x != 0 || g_mc.pub_look_y != 0));

	if (!force && !changed) {
		return;
	}
	if (!force && (now_ms - g_mc.last_look_publish_ms) < min_ms) {
		return;
	}

	(void)app_event_publish_look_target_with_timestamp(
		g_mc.look_target_x, g_mc.look_target_y,
		g_mc.look_confidence, now_ms);

	g_mc.last_look_publish_ms = now_ms;
	g_mc.pub_look_x = g_mc.look_target_x;
	g_mc.pub_look_y = g_mc.look_target_y;
	g_mc.pub_look_conf = g_mc.look_confidence;
}

static void sync_detector_from_public_state(int64_t now_ms)
{
	g_mc.in_hand_det.pickup_confidence = g_mc.pickup_confidence;
	g_mc.in_hand_det.in_hand_confidence = g_mc.in_hand_confidence;
	g_mc.in_hand_det.pickup_confidence_q8 =
		(int32_t)g_mc.pickup_confidence * 256;
	g_mc.in_hand_det.in_hand_confidence_q8 =
		(int32_t)g_mc.in_hand_confidence * 256;
	g_mc.in_hand_det.in_hand = g_mc.in_hand;
	if (g_mc.in_hand) {
		g_mc.in_hand_det.state = IN_HAND_DETECTOR_IN_HAND;
		g_mc.in_hand_det.last_in_hand_ms = now_ms;
	} else if (g_mc.in_hand_det.state == IN_HAND_DETECTOR_IN_HAND ||
		   g_mc.in_hand_det.state == IN_HAND_DETECTOR_WALKING) {
		g_mc.in_hand_det.exit_candidate_since_ms = now_ms;
	}
}

static void emit_detector_events(const struct in_hand_detector_output *det,
				 int64_t now_ms)
{
	if (det->pickup_candidate) {
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKUP_CANDIDATE,
						       det->pickup_confidence,
						       now_ms);
	}
	if (det->picked_up) {
		g_mc.last_pickup_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKED_UP,
						       det->pickup_confidence,
						       now_ms);
	}
	if (det->in_hand_enter) {
		g_mc.last_in_hand_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_ENTER,
						       det->in_hand_confidence,
						       now_ms);
	}
	if (det->in_hand_exit) {
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_EXIT,
						       det->in_hand_confidence,
						       now_ms);
	}
}

static bool uses_idle_polling(void)
{
	const struct motion_sensor_capabilities *caps = motion_sensor_get_capabilities();

	return caps->backend_ready && !caps->backend_has_tilt;
}

static void schedule_now(void)
{
	(void)k_work_reschedule(&g_mc.sample_work, K_NO_WAIT);
}

static void switch_mode(enum motion_classifier_mode mode, int64_t now_ms)
{
	enum motion_sensor_mode sensor_mode;

	if (mode == g_mc.mode) {
		if (mode == MODE_IDLE && uses_idle_polling()) {
			(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(IDLE_POLL_MS));
		}
		return;
	}

	g_mc.mode = mode;
	g_mc.last_mode_change_ms = now_ms;

	switch (mode) {
	case MODE_OFF:
		(void)k_work_cancel_delayable(&g_mc.sample_work);
		(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_DISABLED);
		return;
	case MODE_IDLE:
		(void)motion_sensor_set_mode(MOTION_SENSOR_MODE_IDLE);
		if (uses_idle_polling()) {
			(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(IDLE_POLL_MS));
		} else {
			(void)k_work_cancel_delayable(&g_mc.sample_work);
		}
		return;
	case MODE_ACTIVE_WINDOW:
		sensor_mode = MOTION_SENSOR_MODE_ACTIVE;
		break;
	case MODE_WALK_MAINTAIN:
		sensor_mode = MOTION_SENSOR_MODE_WALK_MAINTAIN;
		break;
	case MODE_IN_HAND_TRACK:
		sensor_mode = MOTION_SENSOR_MODE_IN_HAND_TRACK;
		break;
	default:
		return;
	}

	(void)motion_sensor_set_mode(sensor_mode);
	schedule_now();
}

static void debug_log(const struct imu_processed_frame *frame,
		      const struct in_hand_detector_output *det,
		      const struct imu_window_summary *long_win)
{
	if (!g_mc.debug_logging ||
	    (frame->now_ms - g_mc.last_debug_log_ms) < 1000LL) {
		return;
	}

	LOG_INF("MC mode=%d a=%d,%d,%d g=%d/%d,%d,%d lin=%u sm=%u "
		"jerk=%u gyro=%u/%u surf=%u/%u walk=%u/%d cad=%u "
		"pickup=%u hand=%u/%d rot=%u look=%u/%d,%d chaos=%u det=%d",
		g_mc.mode,
		frame->accel_x_mg, frame->accel_y_mg, frame->accel_z_mg,
		frame->gravity_valid ? 1 : 0,
		frame->gravity_x_mg, frame->gravity_y_mg, frame->gravity_z_mg,
		frame->linear_motion_mg, frame->smooth_linear_motion_mg,
		frame->jerk_mg_per_s, frame->gyro_sum_mdps,
		frame->smooth_gyro_sum_mdps,
		frame->surface_still_score, long_win->surface_still_score,
		g_mc.walking_confidence, g_mc.walking_active ? 1 : 0,
		g_mc.cadence_confidence, g_mc.pickup_confidence,
		g_mc.in_hand_confidence, g_mc.in_hand ? 1 : 0,
		g_mc.rotation_confidence,
		g_mc.look_confidence, g_mc.look_target_x, g_mc.look_target_y,
		g_mc.chaos_confidence, det->state);

	g_mc.last_debug_log_ms = frame->now_ms;
}

static void publish_motion_wake(uint16_t motion_mg, int64_t now_ms)
{
	if ((now_ms - g_mc.last_motion_wake_ms) >= 750LL) {
		(void)app_event_publish_with_timestamp(APP_EVENT_MOTION_WAKE,
						       motion_mg, now_ms);
	}
	g_mc.last_motion_wake_ms = now_ms;
}

static void process_idle_sample(const struct motion_sensor_sample *sample)
{
	struct imu_processed_frame frame;
	uint16_t accel_delta = 0U;

	if (!build_frame(sample, &frame)) {
		g_mc.last_sample = *sample;
		g_mc.have_last_sample = true;
		(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(IDLE_POLL_MS));
		return;
	}

	if (g_mc.have_last_sample) {
		accel_delta = vec_distance_mg(sample->accel_mg_x, sample->accel_mg_y,
					      sample->accel_mg_z,
					      g_mc.last_sample.accel_mg_x,
					      g_mc.last_sample.accel_mg_y,
					      g_mc.last_sample.accel_mg_z);
	}

	g_mc.prev_linear_x = frame.linear_x_mg;
	g_mc.prev_linear_y = frame.linear_y_mg;
	g_mc.prev_linear_z = frame.linear_z_mg;
	g_mc.have_prev_linear = true;
	g_mc.last_sample = *sample;
	g_mc.have_last_sample = true;

	if ((frame.linear_motion_mg >= IDLE_WAKE_LINEAR_MG &&
	     accel_delta >= 70U) ||
	    accel_delta >= IDLE_WAKE_DELTA_MG) {
		publish_motion_wake(frame.linear_motion_mg, frame.now_ms);
		g_mc.active_until_ms =
			frame.now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
		switch_mode(MODE_ACTIVE_WINDOW, frame.now_ms);
		return;
	}

	(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(IDLE_POLL_MS));
}

static void handle_battery_critical(int64_t now_ms)
{
	g_mc.walking_active = false;
	set_walking_confidence(0U);
	g_mc.pickup_confidence = 0U;
	g_mc.in_hand_confidence = 0U;
	g_mc.in_hand = false;
	g_mc.look_confidence = 0U;
	g_mc.look_target_x = move_towards_dt(g_mc.look_target_x, 0, 360, DT_MAX_MS);
	g_mc.look_target_y = move_towards_dt(g_mc.look_target_y, 0, 360, DT_MAX_MS);
	reset_look_reference();
	publish_carry(now_ms, true);
	publish_look(now_ms, true);
	switch_mode(MODE_IDLE, now_ms);
}

static void sample_work_handler(struct k_work *work)
{
	struct motion_sensor_sample sample;
	struct imu_processed_frame frame;
	struct imu_window_summary short_win;
	struct imu_window_summary long_win;
	struct in_hand_detector_input det_in;
	struct in_hand_detector_output det_out;
	uint16_t hw_steps;
	bool sw_step = false;
	bool rough;
	bool active_motion;
	int err;

	ARG_UNUSED(work);

	if (!g_mc.enabled) {
		switch_mode(MODE_OFF, k_uptime_get());
		return;
	}
	if (g_mc.battery_critical) {
		handle_battery_critical(k_uptime_get());
		return;
	}

	err = motion_sensor_fetch_sample(&sample);
	if (err != 0) {
		LOG_WRN("Motion sample fetch failed (%d)", err);
		switch_mode(MODE_IDLE, k_uptime_get());
		return;
	}

	g_mc.last_motion_sample_ms = sample.timestamp_ms;

	if (g_mc.mode == MODE_IDLE) {
		process_idle_sample(&sample);
		return;
	}

	if (!build_frame(&sample, &frame)) {
		g_mc.last_sample = sample;
		g_mc.have_last_sample = true;
		(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(sample_period_ms()));
		return;
	}

	append_frame(&frame);
	summarise_window(&short_win, frame.now_ms, FEATURE_SHORT_WINDOW_MS);
	summarise_window(&long_win, frame.now_ms, FEATURE_LONG_WINDOW_MS);

	rough = is_rough_motion(&frame, &short_win);
	hw_steps = read_hw_steps(frame.now_ms);
	if (!motion_sensor_get_capabilities()->backend_has_hw_step_counter) {
		sw_step = detect_sw_step(&frame, &short_win, rough);
	}

	g_mc.cadence_confidence = compute_cadence_confidence(frame.now_ms);
	frame.cadence_score = g_mc.cadence_confidence;
	short_win.cadence_score = g_mc.cadence_confidence;
	long_win.cadence_score = g_mc.cadence_confidence;
	update_walking(sw_step || (hw_steps > 0U), &frame, &short_win);

	g_mc.stability_confidence =
		(uint8_t)(((uint16_t)short_win.stability_score +
			   (uint16_t)long_win.stability_score) / 2U);
	g_mc.chaos_confidence =
		MAX(frame.chaos_score, short_win.chaos_score);
	g_mc.surface_still_confidence =
		(uint8_t)(((uint16_t)long_win.surface_still_score * 3U +
			   (uint16_t)frame.surface_still_score) / 4U);
	g_mc.hand_motion_confidence =
		MAX(frame.hand_motion_score, short_win.hand_motion_score);
	g_mc.rotation_confidence =
		MAX(frame.rotation_score, short_win.rotation_score);

	memset(&det_in, 0, sizeof(det_in));
	det_in.dt_ms = frame.dt_ms;
	det_in.gravity_x = frame.gravity_x_mg;
	det_in.gravity_y = frame.gravity_y_mg;
	det_in.gravity_z = frame.gravity_z_mg;
	det_in.motion_mg = MAX(frame.linear_motion_mg,
			       short_win.avg_linear_motion_mg);
	det_in.smooth_motion_mg = short_win.avg_smooth_linear_motion_mg;
	det_in.jerk_mg = MAX(frame.jerk_mg_per_s,
			     short_win.avg_jerk_mg_per_s);
	det_in.orientation_delta_mg = short_win.avg_orientation_delta_mg;
	det_in.orientation_rate_mg = MAX(frame.orientation_rate_mg_per_s,
					 short_win.avg_orientation_rate_mg_per_s);
	det_in.gyro_sum_mdps = MAX(frame.gyro_sum_mdps,
				   short_win.avg_gyro_sum_mdps);
	det_in.walking_confidence = g_mc.walking_confidence;
	det_in.cadence_confidence = g_mc.cadence_confidence;
	det_in.stability_confidence = g_mc.stability_confidence;
	det_in.chaos_confidence = g_mc.chaos_confidence;
	det_in.surface_still_confidence = g_mc.surface_still_confidence;
	det_in.hand_motion_confidence = g_mc.hand_motion_confidence;
	det_in.rotation_confidence = g_mc.rotation_confidence;
	det_in.walking_active = g_mc.walking_active;
	det_in.rough_motion = rough;
	det_in.motion_wake =
		(frame.now_ms - g_mc.last_motion_wake_ms) <= 1500LL;
	det_in.now_ms = frame.now_ms;

	in_hand_detector_process(&g_mc.in_hand_det, &det_in, &det_out);
	g_mc.pickup_confidence = det_out.pickup_confidence;
	g_mc.in_hand_confidence = det_out.in_hand_confidence;
	g_mc.in_hand = det_out.in_hand;
	if (det_out.state == IN_HAND_DETECTOR_SURFACE_STILL) {
		g_mc.last_still_ms = frame.now_ms;
	}

	check_shake(&frame, &short_win, det_out.in_hand,
		    det_out.in_hand_confidence);
	emit_detector_events(&det_out, frame.now_ms);
	update_look_reference(&det_out, frame.now_ms);
	update_look_target(&det_out, &frame, &short_win);

	publish_carry(frame.now_ms,
		      det_out.pickup_candidate || det_out.picked_up ||
		      det_out.in_hand_enter || det_out.in_hand_exit);
	publish_look(frame.now_ms, false);

	active_motion = frame.linear_motion_mg >= ACTIVE_MOTION_MG ||
			frame.rotation_score >= 28U ||
			frame.hand_motion_score >= 35U;
	if (active_motion) {
		g_mc.last_active_motion_ms = frame.now_ms;
	}

	g_mc.prev_linear_x = frame.linear_x_mg;
	g_mc.prev_linear_y = frame.linear_y_mg;
	g_mc.prev_linear_z = frame.linear_z_mg;
	g_mc.have_prev_linear = true;
	g_mc.last_sample = sample;
	g_mc.have_last_sample = true;

	debug_log(&frame, &det_out, &long_win);

	if (g_mc.walking_active) {
		switch_mode(MODE_WALK_MAINTAIN, frame.now_ms);
	} else if (g_mc.in_hand || g_mc.in_hand_confidence >= 35U ||
		   g_mc.pickup_confidence >= 30U) {
		g_mc.active_until_ms =
			MAX(g_mc.active_until_ms,
			    frame.now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS);
		switch_mode(MODE_IN_HAND_TRACK, frame.now_ms);
	} else if (frame.now_ms < g_mc.active_until_ms) {
		switch_mode(MODE_ACTIVE_WINDOW, frame.now_ms);
	} else if (g_mc.surface_still_confidence >= SURFACE_STILL_SCORE &&
		   long_win.count >= 6U) {
		switch_mode(MODE_IDLE, frame.now_ms);
		return;
	} else if (active_motion &&
		   (frame.now_ms - g_mc.last_active_motion_ms) < 1500LL) {
		g_mc.active_until_ms = frame.now_ms + 1000LL;
		switch_mode(MODE_ACTIVE_WINDOW, frame.now_ms);
	} else {
		switch_mode(MODE_IDLE, frame.now_ms);
		return;
	}

	(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(sample_period_ms()));
}

static void sensor_event_handler(uint32_t events, void *user_data)
{
	int64_t now_ms = k_uptime_get();

	ARG_UNUSED(user_data);

	if (!g_mc.enabled || g_mc.battery_critical) {
		return;
	}

	publish_motion_wake((uint16_t)events, now_ms);
	g_mc.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
	switch_mode(MODE_ACTIVE_WINDOW, now_ms);
	schedule_now();
}

int motion_classifier_init(int64_t now_ms)
{
	int err;

	memset(&g_mc, 0, sizeof(g_mc));
	k_work_init_delayable(&g_mc.sample_work, sample_work_handler);
	in_hand_detector_init(&g_mc.in_hand_det, now_ms);
	g_mc.mode = MODE_OFF;
	g_mc.battery_percent = -1;
	g_mc.pub_look_conf = UINT8_MAX;

	if (!IS_ENABLED(CONFIG_KERFUR_MOTION)) {
		LOG_INF("Motion classifier disabled by Kconfig");
		return 0;
	}

	err = motion_sensor_init();
	if (err != 0) {
		LOG_WRN("Motion classifier running without sensor (%d)", err);
		g_mc.initialized = true;
		g_mc.enabled = false;
		return 0;
	}

	(void)motion_sensor_set_event_handler(sensor_event_handler, NULL);
	g_mc.initialized = true;
	g_mc.enabled = true;

	if (uses_idle_polling()) {
		LOG_INF("Motion: polling fallback (no tilt interrupt)");
	}

	switch_mode(MODE_IDLE, now_ms);
	return 0;
}

void motion_classifier_on_event(const struct app_event *event,
				const struct pet_state *state)
{
	if (event == NULL || !g_mc.initialized) {
		return;
	}

	switch (event->type) {
	case APP_EVENT_BATTERY_PERCENT_UPDATE:
		g_mc.battery_percent = event->payload.battery_percent.known ?
			event->payload.battery_percent.percent : -1;
		g_mc.battery_percent_known = event->payload.battery_percent.known;
		g_mc.battery_low = event->payload.battery_percent.known &&
			(event->payload.battery_percent.percent <= 20);
		g_mc.battery_critical = event->payload.battery_percent.known &&
			(event->payload.battery_percent.percent <= 5);
		if (g_mc.battery_low && g_mc.active_until_ms > 0LL) {
			g_mc.active_until_ms = MIN(g_mc.active_until_ms,
						   event->timestamp_ms + 1500LL);
		}
		if (g_mc.battery_critical) {
			schedule_now();
		}
		break;
	case APP_EVENT_BATTERY_LOW:
		g_mc.battery_low = true;
		break;
	case APP_EVENT_BATTERY_CRITICAL:
		g_mc.battery_critical = true;
		reset_look_reference();
		schedule_now();
		break;
	case APP_EVENT_CHARGER_CONNECTED:
		g_mc.charging = true;
		break;
	case APP_EVENT_CHARGER_DISCONNECTED:
		g_mc.charging = false;
		break;
	case APP_EVENT_WALKING_START:
		g_mc.walking_active = true;
		set_walking_confidence((event->param > 0) ?
				       clamp_u8(event->param) :
				       CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD);
		g_mc.active_until_ms = event->timestamp_ms + 1500LL;
		g_mc.suppress_look_until_ms =
			max64(g_mc.suppress_look_until_ms,
			      event->timestamp_ms + LOOK_SUPPRESS_AFTER_WALK_MS);
		break;
	case APP_EVENT_WALKING_STOP:
		g_mc.walking_active = false;
		set_walking_confidence(clamp_u8(event->param));
		break;
	case APP_EVENT_CARRY_STATE_UPDATE:
		g_mc.in_hand = event->payload.carry_state.in_hand;
		g_mc.pickup_confidence = event->payload.carry_state.pickup_confidence;
		g_mc.in_hand_confidence =
			event->payload.carry_state.in_hand_confidence;
		set_walking_confidence(event->payload.carry_state.walking_confidence);
		sync_detector_from_public_state(event->timestamp_ms);
		if (!g_mc.in_hand && g_mc.look_idle_since_ms == 0LL) {
			g_mc.look_idle_since_ms = event->timestamp_ms;
		}
		break;
	case APP_EVENT_PICKED_UP:
		g_mc.last_pickup_ms = event->timestamp_ms;
		g_mc.pickup_confidence =
			MAX(g_mc.pickup_confidence, clamp_u8(event->param));
		sync_detector_from_public_state(event->timestamp_ms);
		g_mc.active_until_ms =
			event->timestamp_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
		break;
	case APP_EVENT_IN_HAND_ENTER:
		g_mc.last_in_hand_ms = event->timestamp_ms;
		g_mc.in_hand = true;
		g_mc.in_hand_confidence =
			MAX(g_mc.in_hand_confidence, clamp_u8(event->param));
		sync_detector_from_public_state(event->timestamp_ms);
		break;
	case APP_EVENT_IN_HAND_EXIT:
		g_mc.in_hand = false;
		g_mc.in_hand_confidence =
			MIN(g_mc.in_hand_confidence, clamp_u8(event->param));
		sync_detector_from_public_state(event->timestamp_ms);
		if (g_mc.look_idle_since_ms == 0LL) {
			g_mc.look_idle_since_ms = event->timestamp_ms;
		}
		break;
	default:
		break;
	}

	if (state != NULL && !g_mc.enabled) {
		g_mc.battery_low = state->battery_low;
		g_mc.battery_critical = state->battery_critical;
	}
}

void motion_classifier_set_debug_logging(bool enabled)
{
	g_mc.debug_logging = enabled;
	if (enabled) {
		g_mc.last_debug_log_ms = 0LL;
	}
}

bool motion_classifier_is_debug_logging(void)
{
	return g_mc.debug_logging;
}

bool motion_classifier_is_enabled(void)
{
	return g_mc.enabled;
}
