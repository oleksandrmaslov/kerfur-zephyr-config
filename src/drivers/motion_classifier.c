/*
 * Motion classifier — LSM6DSO-focused rewrite.
 *
 * Architecture:
 *   IDLE  ──(wake/tilt interrupt)──▷  ACTIVE_WINDOW  ──▷  decision
 *                                         │
 *                              ┌──────────┼──────────┐
 *                              ▼          ▼          ▼
 *                         WALK_MAINTAIN  IN_HAND   IDLE
 *
 * Key principles:
 * - Gravity is validated before use (boot-safe).
 * - Step detection uses HW pedometer when available, SW peak fallback otherwise.
 * - Walking confidence has hysteresis (start 70, stop 45).
 * - In-hand detection is delegated to in_hand_detector module.
 * - Gaze tracking uses gravity-relative tilt with a captured neutral reference.
 * - Shake detection is suppressed when device is confidently in-hand.
 * - Mode transitions have a soft lingering window to avoid detection gaps.
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

/* ── Tuning constants ─────────────────────────────────────────────────── */

#define IDLE_POLL_MS               800
#define IDLE_WAKE_THRESHOLD_MG     250U
#define IDLE_WAKE_SMOOTH_MG        180U

#define FEATURE_WINDOW_FRAMES      12U

/* Step detection (software fallback). */
#define STEP_PEAK_MG               300U
#define STEP_MIN_INTERVAL_MS       300LL
#define STEP_MAX_INTERVAL_MS       850LL

/* Gravity estimation. */
#define GRAVITY_MIN_MAG_SQ         722500U   /* 850 mg squared */
#define GRAVITY_MAX_MAG_SQ         1322500U  /* 1150 mg squared */
#define GRAVITY_FAST_ALPHA         6
#define GRAVITY_SLOW_ALPHA         14

/* Gaze. */
#define GAZE_DEADBAND              3
#define GAZE_MAX_DELTA_NORMAL      15
#define GAZE_MAX_DELTA_LOW_BATT    8
#define LOOK_SUPPRESS_AFTER_ROUGH_MS 800LL
#define LOOK_SUPPRESS_AFTER_WALK_MS  500LL
#define LOOK_REFERENCE_ADAPT_ALPHA   60

/* Shake cooldowns. */
#define SHAKE_COOLDOWN_LIGHT_MS    800
#define SHAKE_COOLDOWN_PLAY_MS     900
#define SHAKE_COOLDOWN_ROUGH_MS    900
#define SHAKE_COOLDOWN_IMPACT_MS   1200

/* ── Feature frame for windowed classification ────────────────────────── */

struct motion_feature_frame {
	uint16_t motion_mg;
	uint16_t smooth_motion_mg;
	uint16_t jerk_mg;
	uint16_t orientation_delta_mg;
	uint16_t orientation_rate_mg;
	uint32_t gyro_sum_mdps;
	int64_t  timestamp_ms;
};

struct motion_feature_summary {
	uint16_t avg_motion_mg;
	uint16_t avg_jerk_mg;
	uint16_t avg_orientation_delta_mg;
	uint32_t avg_gyro_sum_mdps;
	uint8_t  stability_confidence;
	uint8_t  chaos_confidence;
	uint8_t  cadence_confidence;
};

/* ── Classifier modes ─────────────────────────────────────────────────── */

enum motion_classifier_mode {
	MODE_OFF = 0,
	MODE_IDLE,
	MODE_ACTIVE_WINDOW,
	MODE_WALK_MAINTAIN,
	MODE_IN_HAND_TRACK,
};

/* ── Global state ─────────────────────────────────────────────────────── */

struct motion_classifier_state {
	struct k_work_delayable sample_work;
	struct in_hand_detector in_hand_det;
	enum motion_classifier_mode mode;
	struct motion_sensor_sample last_sample;

	/* Flags. */
	bool enabled;
	bool initialized;
	bool debug_logging;
	bool battery_low;
	bool battery_critical;
	bool battery_percent_known;
	bool charging;
	bool gravity_valid;
	bool hw_counter_valid;
	bool pending_steps_from_hw;
	bool walking_active;
	bool in_hand;
	bool over_step_threshold;
	bool look_reference_valid;

	int8_t  battery_percent;

	/* Gravity (low-pass). */
	int16_t gravity_x;
	int16_t gravity_y;
	int16_t gravity_z;

	/* Previous linear accel (for jerk). */
	int16_t prev_linear_x;
	int16_t prev_linear_y;
	int16_t prev_linear_z;

	/* Gaze reference. */
	int16_t look_ref_x;
	int16_t look_ref_y;
	int16_t look_ref_z;
	int16_t look_target_x;
	int16_t look_target_y;

	/* Confidences. */
	uint8_t walking_confidence;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t look_confidence;
	uint8_t stability_confidence;
	uint8_t chaos_confidence;
	uint8_t cadence_confidence;

	/* Step tracking. */
	uint16_t last_hw_step_counter;
	uint32_t pending_steps;
	uint8_t  recent_step_count;
	uint8_t  recent_step_index;
	int64_t  recent_step_ms[6];
	int64_t  last_peak_ms;

	/* Feature window. */
	struct motion_feature_frame frames[FEATURE_WINDOW_FRAMES];
	uint8_t frame_count;
	uint8_t frame_index;

	/* Timestamps. */
	int64_t active_until_ms;
	int64_t last_motion_wake_ms;
	int64_t suppress_look_until_ms;
	int64_t last_shake_event_ms;
	int64_t last_active_motion_ms;
	int64_t last_carry_publish_ms;
	int64_t last_look_publish_ms;
	int64_t last_debug_log_ms;
	int64_t last_pickup_ms;
	int64_t last_in_hand_ms;
	int64_t last_motion_sample_ms;
	int64_t last_still_ms;

	/* Last-published (for change detection). */
	bool    pub_in_hand;
	uint8_t pub_pickup_conf;
	uint8_t pub_in_hand_conf;
	uint8_t pub_walk_conf;
	int16_t pub_look_x;
	int16_t pub_look_y;
	uint8_t pub_look_conf;
};

static struct motion_classifier_state g_mc;

/* ── Utilities ────────────────────────────────────────────────────────── */

static uint8_t clamp_u8(int v) { return (uint8_t)CLAMP(v, 0, 100); }
static int16_t clamp_look(int v) { return (int16_t)CLAMP(v, -100, 100); }
static int     abs16(int16_t v) { return (v < 0) ? -v : v; }
static int64_t max64(int64_t a, int64_t b) { return (a > b) ? a : b; }

static int sample_period_ms(void)
{
	int hz = CONFIG_KERFUR_MOTION_ACTIVE_ODR_HZ;
	return (hz > 0) ? MAX(20, 1000 / hz) : 100;
}

/* ── Axis normalisation ───────────────────────────────────────────────── */

static void normalise_sample(struct motion_sensor_sample *s)
{
	int16_t ax, ay, az, gx, gy, gz;

	if (s == NULL) {
		return;
	}
	ax = s->accel_mg_x;  ay = s->accel_mg_y;  az = s->accel_mg_z;
	gx = s->gyro_mdps_x; gy = s->gyro_mdps_y; gz = s->gyro_mdps_z;

	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_SWAP_XY)) {
		int16_t t;
		t = ax; ax = ay; ay = t;
		t = gx; gx = gy; gy = t;
	}
	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_X)) { ax = -ax; gx = -gx; }
	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_Y)) { ay = -ay; gy = -gy; }
	if (IS_ENABLED(CONFIG_KERFUR_MOTION_AXIS_INVERT_Z)) { az = -az; gz = -gz; }

	s->accel_mg_x = ax; s->accel_mg_y = ay; s->accel_mg_z = az;
	s->gyro_mdps_x = gx; s->gyro_mdps_y = gy; s->gyro_mdps_z = gz;
}

/* ── Gravity estimation ───────────────────────────────────────────────── */

static bool gravity_sample_plausible(const struct motion_sensor_sample *s)
{
	int32_t x = s->accel_mg_x, y = s->accel_mg_y, z = s->accel_mg_z;
	uint32_t mag_sq = (uint32_t)(x * x + y * y + z * z);
	return (mag_sq >= GRAVITY_MIN_MAG_SQ) && (mag_sq <= GRAVITY_MAX_MAG_SQ);
}

static void update_gravity(const struct motion_sensor_sample *s)
{
	bool plausible = gravity_sample_plausible(s);
	int alpha;

	if (!g_mc.gravity_valid) {
		if (plausible) {
			g_mc.gravity_x = s->accel_mg_x;
			g_mc.gravity_y = s->accel_mg_y;
			g_mc.gravity_z = s->accel_mg_z;
			g_mc.gravity_valid = true;
		}
		return;
	}

	alpha = plausible ? GRAVITY_FAST_ALPHA : GRAVITY_SLOW_ALPHA;
	g_mc.gravity_x += (s->accel_mg_x - g_mc.gravity_x) / alpha;
	g_mc.gravity_y += (s->accel_mg_y - g_mc.gravity_y) / alpha;
	g_mc.gravity_z += (s->accel_mg_z - g_mc.gravity_z) / alpha;
}

/* ── Feature window ───────────────────────────────────────────────────── */

static void append_frame(const struct motion_feature_frame *f)
{
	g_mc.frames[g_mc.frame_index] = *f;
	g_mc.frame_index = (g_mc.frame_index + 1U) % FEATURE_WINDOW_FRAMES;
	if (g_mc.frame_count < FEATURE_WINDOW_FRAMES) {
		g_mc.frame_count++;
	}
}

static void summarise_features(struct motion_feature_summary *out, int64_t now_ms)
{
	uint32_t sum_motion = 0, sum_jerk = 0, sum_orient = 0;
	uint64_t sum_gyro = 0;
	uint8_t  stable = 0, chaotic = 0, smooth = 0;
	uint8_t  count = 0, i;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < g_mc.frame_count; i++) {
		const struct motion_feature_frame *f = &g_mc.frames[i];
		if ((now_ms - f->timestamp_ms) > 2000LL) {
			continue;
		}
		sum_motion += f->motion_mg;
		sum_jerk   += f->jerk_mg;
		sum_orient += f->orientation_delta_mg;
		sum_gyro   += f->gyro_sum_mdps;

		/* Classify each frame. */
		if (f->motion_mg >= 15U && f->motion_mg <= 280U &&
		    f->jerk_mg <= 250U && f->gyro_sum_mdps <= 28000U) {
			stable++;
		}
		if (f->orientation_delta_mg >= 10U && f->orientation_delta_mg <= 260U &&
		    f->jerk_mg <= 260U && f->gyro_sum_mdps <= 32000U) {
			smooth++;
		}
		if (f->motion_mg >= 600U || f->jerk_mg >= 350U || f->gyro_sum_mdps >= 45000U) {
			chaotic++;
		}
		count++;
	}

	if (count == 0U) {
		return;
	}

	out->avg_motion_mg = (uint16_t)(sum_motion / count);
	out->avg_jerk_mg   = (uint16_t)(sum_jerk / count);
	out->avg_orientation_delta_mg = (uint16_t)(sum_orient / count);
	out->avg_gyro_sum_mdps = (uint32_t)(sum_gyro / count);

	out->stability_confidence = clamp_u8(
		20 + stable * 7 + smooth * 4
		- chaotic * 9
		- (int)MAX(0, (int)out->avg_jerk_mg - 200) / 6
		- (int)MAX(0, (int)out->avg_gyro_sum_mdps - 24000) / 2000);

	out->chaos_confidence = clamp_u8(
		chaotic * 15
		+ (int)MAX(0, (int)out->avg_jerk_mg - 160) / 4
		+ (int)MAX(0, (int)out->avg_gyro_sum_mdps - 18000) / 1600
		+ (int)MAX(0, (int)out->avg_motion_mg - 260) / 6
		- smooth * 3);

	/* Cadence confidence is computed separately after step tracking init. */
	out->cadence_confidence = 0U;
}

/* ── Step tracking ────────────────────────────────────────────────────── */

static void note_step(int64_t ms)
{
	g_mc.recent_step_ms[g_mc.recent_step_index] = ms;
	g_mc.recent_step_index = (g_mc.recent_step_index + 1U) % ARRAY_SIZE(g_mc.recent_step_ms);
	if (g_mc.recent_step_count < ARRAY_SIZE(g_mc.recent_step_ms)) {
		g_mc.recent_step_count++;
	}
	g_mc.last_peak_ms = MAX(g_mc.last_peak_ms, ms);
}

static uint8_t count_recent_steps(int64_t now_ms)
{
	uint8_t n = 0, i;
	for (i = 0; i < ARRAY_SIZE(g_mc.recent_step_ms); i++) {
		if (g_mc.recent_step_ms[i] > 0LL &&
		    (now_ms - g_mc.recent_step_ms[i]) <= 2200LL) {
			n++;
		}
	}
	return n;
}

static uint8_t compute_cadence_confidence(int64_t now_ms)
{
	int64_t intervals[5];
	uint8_t valid = 0, recent = count_recent_steps(now_ms);
	int64_t sum = 0, avg, err_sum = 0;
	uint8_t i, prev_idx;

	if (recent < 3U) {
		return 0U;
	}

	/* Build interval list from newest steps backwards. */
	for (i = 0; i < ARRAY_SIZE(g_mc.recent_step_ms) && valid < 5U; i++) {
		prev_idx = (g_mc.recent_step_index + ARRAY_SIZE(g_mc.recent_step_ms) - 1U - i) %
			   ARRAY_SIZE(g_mc.recent_step_ms);
		if (i > 0 && g_mc.recent_step_ms[prev_idx] > 0LL) {
			uint8_t curr_idx = (prev_idx + 1U) % ARRAY_SIZE(g_mc.recent_step_ms);
			int64_t dt = g_mc.recent_step_ms[curr_idx] - g_mc.recent_step_ms[prev_idx];
			if (dt >= 250LL && dt <= 1100LL) {
				intervals[valid++] = dt;
				sum += dt;
			}
		}
	}

	if (valid < 2U) {
		return 0U;
	}

	avg = sum / valid;
	if (avg < 280LL || avg > 900LL) {
		return 0U;
	}

	for (i = 0; i < valid; i++) {
		int64_t d = intervals[i] - avg;
		err_sum += (d < 0) ? -d : d;
	}

	return clamp_u8((int)(recent * 14U + 26U) - (int)(err_sum / 4));
}

static void flush_steps(int64_t now_ms)
{
	if (g_mc.pending_steps == 0U) {
		return;
	}
	(void)app_event_publish_step_batch_with_timestamp(
		(int32_t)g_mc.pending_steps,
		g_mc.last_hw_step_counter,
		g_mc.walking_confidence,
		g_mc.pending_steps_from_hw,
		now_ms);
	g_mc.pending_steps = 0U;
	g_mc.pending_steps_from_hw = false;
}

static uint16_t read_hw_steps(int64_t now_ms)
{
	uint16_t hw = 0U, delta;
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
	if (delta > 128U) {
		g_mc.last_hw_step_counter = hw;
		return 0U;  /* Spurious. */
	}
	g_mc.last_hw_step_counter = hw;
	g_mc.pending_steps += delta;
	g_mc.pending_steps_from_hw = true;
	for (uint16_t s = 0; s < delta; s++) {
		note_step(now_ms);
	}
	if (g_mc.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
		flush_steps(now_ms);
	}
	return delta;
}

static bool detect_sw_step(int16_t lx, int16_t ly, int16_t lz,
			   bool rough, int64_t now_ms)
{
	uint16_t intensity = (uint16_t)(abs16(lx) + abs16(ly) + abs16(lz));
	bool over = intensity >= STEP_PEAK_MG;
	int64_t dt = now_ms - g_mc.last_peak_ms;
	bool detected = false;

	if (!g_mc.over_step_threshold && over && !rough &&
	    (g_mc.last_peak_ms == 0LL ||
	     (dt >= STEP_MIN_INTERVAL_MS && dt <= STEP_MAX_INTERVAL_MS))) {
		detected = true;
		g_mc.last_peak_ms = now_ms;
		note_step(now_ms);
		g_mc.pending_steps++;
		if (g_mc.pending_steps >= CONFIG_KERFUR_MOTION_STEP_BATCH_SIZE) {
			flush_steps(now_ms);
		}
	}
	g_mc.over_step_threshold = over;
	return detected;
}

/* ── Walking confidence ───────────────────────────────────────────────── */

static void update_walking(bool step, const struct motion_feature_summary *fs, int64_t now_ms)
{
	uint8_t recent = count_recent_steps(now_ms);
	int delta = 0;

	if (step) {
		delta += 3;
		delta += (int)fs->cadence_confidence / 12;
		delta += MIN((int)recent * 2, 8);
		delta -= (int)fs->chaos_confidence / 10;
		delta += (recent >= 4U) ? 4 : (recent >= 2U) ? 2 : 0;
	} else if (fs->chaos_confidence >= 70U) {
		delta -= 15;
	} else if (fs->cadence_confidence >= 55U && recent >= 3U) {
		delta -= 1;
	} else if (g_mc.last_peak_ms > 0LL && (now_ms - g_mc.last_peak_ms) > 1400LL) {
		delta -= 6;
	} else {
		delta -= 3;
	}

	/* Steady walking bonus. */
	if (fs->avg_motion_mg >= 220U && fs->avg_motion_mg <= 550U &&
	    fs->chaos_confidence <= 50U && fs->cadence_confidence >= 35U) {
		delta += 2;
	}

	g_mc.walking_confidence = clamp_u8((int)g_mc.walking_confidence + delta);

	if (!g_mc.walking_active &&
	    g_mc.walking_confidence >= CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD) {
		g_mc.walking_active = true;
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_START,
						       g_mc.walking_confidence, now_ms);
	} else if (g_mc.walking_active &&
		   g_mc.walking_confidence <= CONFIG_KERFUR_MOTION_WALK_STOP_THRESHOLD) {
		g_mc.walking_active = false;
		flush_steps(now_ms);
		(void)app_event_publish_with_timestamp(APP_EVENT_WALKING_STOP,
						       g_mc.walking_confidence, now_ms);
	}
}

/* ── Shake detection ──────────────────────────────────────────────────── */

static void check_shake(uint16_t motion_mg, uint32_t gyro,
			uint8_t stability, uint8_t chaos,
			bool in_hand, uint8_t in_hand_conf, int64_t now_ms)
{
	enum app_event_type type = APP_EVENT_COUNT;
	int cooldown = 0;
	int64_t suppress = 0LL;

	/* Thresholds tuned so normal hand-twisting does NOT trigger shakes.
	 * Gyro thresholds are high because rotation in hand easily hits 40-60k mdps. */
	if (motion_mg >= 2200U || gyro >= 120000U) {
		type = APP_EVENT_IMPACT;
		cooldown = SHAKE_COOLDOWN_IMPACT_MS;
		suppress = now_ms + 1400LL;
	} else if (motion_mg >= 1500U || gyro >= 95000U) {
		type = APP_EVENT_SHAKE_ROUGH;
		cooldown = SHAKE_COOLDOWN_ROUGH_MS;
		suppress = now_ms + 1200LL;
	} else if (motion_mg >= 1000U || gyro >= 75000U) {
		type = APP_EVENT_SHAKE_PLAY;
		cooldown = SHAKE_COOLDOWN_PLAY_MS;
	} else if (motion_mg >= 750U || gyro >= 55000U) {
		type = APP_EVENT_SHAKE_LIGHT;
		cooldown = SHAKE_COOLDOWN_LIGHT_MS;
	}

	if (type == APP_EVENT_COUNT) {
		return;
	}

	/* Suppress light/play when in hand or hand-like context. */
	if (type == APP_EVENT_SHAKE_LIGHT || type == APP_EVENT_SHAKE_PLAY) {
		if (in_hand || in_hand_conf >= 30U) {
			return;
		}
	}

	if ((now_ms - g_mc.last_shake_event_ms) < cooldown) {
		return;
	}

	g_mc.last_shake_event_ms = now_ms;
	g_mc.suppress_look_until_ms = max64(g_mc.suppress_look_until_ms, suppress);
	(void)app_event_publish_with_timestamp(type, 0, now_ms);
}

/* ── Gaze tracking ────────────────────────────────────────────────────── */

static void reset_look_reference(void)
{
	g_mc.look_reference_valid = false;
	g_mc.look_ref_x = 0;
	g_mc.look_ref_y = 0;
	g_mc.look_ref_z = 0;
}

static void capture_look_reference(void)
{
	g_mc.look_ref_x = g_mc.gravity_x;
	g_mc.look_ref_y = g_mc.gravity_y;
	g_mc.look_ref_z = g_mc.gravity_z;
	g_mc.look_reference_valid = true;
}

static void update_look_reference(const struct in_hand_detector_output *det,
				  const struct motion_feature_summary *fs)
{
	/* Reset only on confirmed surface still (not just !in_hand). */
	if (det->state == IN_HAND_DETECTOR_SURFACE_STILL && !det->in_hand) {
		reset_look_reference();
		return;
	}

	/* Capture reference on enter, or as soon as any hand-like motion is detected. */
	if (det->in_hand_enter || det->picked_up ||
	    (!g_mc.look_reference_valid && det->in_hand_confidence >= 15U)) {
		capture_look_reference();
		return;
	}

	if (!g_mc.look_reference_valid) {
		return;
	}

	/* Very slowly adapt reference so the "neutral" drifts with the hand position. */
	if (fs->stability_confidence >= 30U &&
	    fs->chaos_confidence <= 50U) {
		g_mc.look_ref_x += (g_mc.gravity_x - g_mc.look_ref_x) / LOOK_REFERENCE_ADAPT_ALPHA;
		g_mc.look_ref_y += (g_mc.gravity_y - g_mc.look_ref_y) / LOOK_REFERENCE_ADAPT_ALPHA;
		g_mc.look_ref_z += (g_mc.gravity_z - g_mc.look_ref_z) / LOOK_REFERENCE_ADAPT_ALPHA;
	}
}

static int16_t move_towards(int16_t cur, int16_t tgt, int16_t max_delta)
{
	int d = tgt - cur;
	if (d > max_delta)  d = max_delta;
	if (d < -max_delta) d = -max_delta;
	return cur + (int16_t)d;
}

static void update_look_target(const struct in_hand_detector_output *det,
			       const struct motion_feature_summary *fs,
			       int64_t now_ms)
{
	int16_t raw_x, raw_y;
	int max_delta;
	bool suppressed;

	suppressed = (now_ms < g_mc.suppress_look_until_ms) ||
		     g_mc.walking_active ||
		     (det->look_confidence < 10U && !det->in_hand) ||
		     !g_mc.look_reference_valid;

	if (suppressed) {
		/* Smoothly return to center. Keep this slow so the pupils don't
		 * snap back the moment confidence dips — at 25Hz a delta of 2
		 * means a full ~2s glide from one extreme to center, which feels
		 * natural to the eye. */
		g_mc.look_target_x = move_towards(g_mc.look_target_x, 0, 2);
		g_mc.look_target_y = move_towards(g_mc.look_target_y, 0, 2);
		g_mc.look_confidence = 0U;
		return;
	}

	raw_x = clamp_look((-(g_mc.gravity_x - g_mc.look_ref_x) * 100) /
			   CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);
	raw_y = clamp_look(((g_mc.gravity_y - g_mc.look_ref_y) * 100) /
			   CONFIG_KERFUR_MOTION_GAZE_TILT_DIVISOR_MG);

	/* Deadband. */
	if (abs16(raw_x) <= GAZE_DEADBAND) { raw_x = 0; }
	if (abs16(raw_y) <= GAZE_DEADBAND) { raw_y = 0; }

	max_delta = g_mc.battery_low ? GAZE_MAX_DELTA_LOW_BATT : GAZE_MAX_DELTA_NORMAL;

	g_mc.look_target_x = move_towards(g_mc.look_target_x, raw_x, (int16_t)max_delta);
	g_mc.look_target_y = move_towards(g_mc.look_target_y, raw_y, (int16_t)max_delta);

	/* Confidence from detector, discounted by noise. */
	g_mc.look_confidence = clamp_u8(
		(int)det->look_confidence -
		(int)fs->chaos_confidence / 3 -
		(int)fs->cadence_confidence / 4);
}

/* ── Event publishing ─────────────────────────────────────────────────── */

static void publish_carry(int64_t now_ms, bool force)
{
	bool changed = force ||
		g_mc.pub_in_hand     != g_mc.in_hand ||
		g_mc.pub_pickup_conf != g_mc.pickup_confidence ||
		g_mc.pub_in_hand_conf!= g_mc.in_hand_confidence ||
		g_mc.pub_walk_conf   != g_mc.walking_confidence;

	if (!changed && (now_ms - g_mc.last_carry_publish_ms) < 250LL) {
		return;
	}

	(void)app_event_publish_carry_state_with_timestamp(
		g_mc.in_hand, g_mc.pickup_confidence,
		g_mc.in_hand_confidence, g_mc.walking_confidence, now_ms);

	g_mc.last_carry_publish_ms = now_ms;
	g_mc.pub_in_hand      = g_mc.in_hand;
	g_mc.pub_pickup_conf  = g_mc.pickup_confidence;
	g_mc.pub_in_hand_conf = g_mc.in_hand_confidence;
	g_mc.pub_walk_conf    = g_mc.walking_confidence;
}

static void publish_look(int64_t now_ms, bool force)
{
	int min_ms = MAX(50, 1000 / CONFIG_KERFUR_MOTION_GAZE_RATE_HZ);
	bool changed = force ||
		abs16(g_mc.look_target_x - g_mc.pub_look_x) >= 2 ||
		abs16(g_mc.look_target_y - g_mc.pub_look_y) >= 2 ||
		g_mc.look_confidence != g_mc.pub_look_conf;

	if (!changed && (now_ms - g_mc.last_look_publish_ms) < min_ms) {
		return;
	}

	(void)app_event_publish_look_target_with_timestamp(
		g_mc.look_target_x, g_mc.look_target_y, g_mc.look_confidence, now_ms);

	g_mc.last_look_publish_ms = now_ms;
	g_mc.pub_look_x    = g_mc.look_target_x;
	g_mc.pub_look_y    = g_mc.look_target_y;
	g_mc.pub_look_conf = g_mc.look_confidence;
}

static void emit_detector_events(const struct in_hand_detector_output *det, int64_t now_ms)
{
	if (det->pickup_candidate) {
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKUP_CANDIDATE,
						       det->pickup_confidence, now_ms);
	}
	if (det->picked_up) {
		g_mc.last_pickup_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_PICKED_UP,
						       det->pickup_confidence, now_ms);
	}
	if (det->in_hand_enter) {
		g_mc.last_in_hand_ms = now_ms;
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_ENTER,
						       det->in_hand_confidence, now_ms);
	}
	if (det->in_hand_exit) {
		(void)app_event_publish_with_timestamp(APP_EVENT_IN_HAND_EXIT,
						       det->in_hand_confidence, now_ms);
	}
}

/* ── Mode management ──────────────────────────────────────────────────── */

static bool uses_idle_polling(void)
{
	const struct motion_sensor_capabilities *caps = motion_sensor_get_capabilities();
	return caps->backend_ready && !caps->backend_has_tilt;
}

static void schedule_now(void)
{
	(void)k_work_reschedule(&g_mc.sample_work, K_NO_WAIT);
}

static void set_mode(enum motion_classifier_mode mode, int64_t now_ms)
{
	enum motion_sensor_mode sensor_mode;

	g_mc.mode = mode;

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
	case MODE_ACTIVE_WINDOW: sensor_mode = MOTION_SENSOR_MODE_ACTIVE;         break;
	case MODE_WALK_MAINTAIN: sensor_mode = MOTION_SENSOR_MODE_WALK_MAINTAIN;  break;
	case MODE_IN_HAND_TRACK: sensor_mode = MOTION_SENSOR_MODE_IN_HAND_TRACK; break;
	default: return;
	}

	(void)motion_sensor_set_mode(sensor_mode);
	g_mc.active_until_ms = max64(g_mc.active_until_ms, now_ms);
	schedule_now();
}

/* ── Debug ────────────────────────────────────────────────────────────── */

static void debug_log(int64_t now_ms)
{
	if (!g_mc.debug_logging || (now_ms - g_mc.last_debug_log_ms) < 1000LL) {
		return;
	}
	LOG_INF("MC walk=%u/%d pickup=%u in_hand=%u/%d look=%u tgt=%d,%d "
		"stab=%u cad=%u chaos=%u ref=%d mode=%d batt=%d",
		g_mc.walking_confidence, g_mc.walking_active ? 1 : 0,
		g_mc.pickup_confidence, g_mc.in_hand_confidence,
		g_mc.in_hand ? 1 : 0, g_mc.look_confidence,
		g_mc.look_target_x, g_mc.look_target_y,
		g_mc.stability_confidence, g_mc.cadence_confidence,
		g_mc.chaos_confidence,
		g_mc.look_reference_valid ? 1 : 0,
		g_mc.mode, g_mc.battery_percent);
	g_mc.last_debug_log_ms = now_ms;
}

/* ── Main sample work handler ─────────────────────────────────────────── */

static void sample_work_handler(struct k_work *work)
{
	struct motion_sensor_sample sample;
	struct in_hand_detector_input det_in;
	struct in_hand_detector_output det_out;
	struct motion_feature_frame frame;
	struct motion_feature_summary summary;
	int16_t lx, ly, lz;
	int16_t prev_gx, prev_gy, prev_gz;
	uint16_t orient_delta, orient_rate, jerk_mg, motion_mg, smooth_mg;
	uint32_t gyro_sum;
	uint16_t hw_steps = 0U;
	bool rough, step = false;
	bool idle_poll = (g_mc.mode == MODE_IDLE);
	int64_t now_ms;
	int err;

	ARG_UNUSED(work);

	if (!g_mc.enabled) {
		set_mode(MODE_OFF, k_uptime_get());
		return;
	}

	if (g_mc.battery_critical) {
		/* Shut down motion processing to save power. */
		g_mc.walking_confidence = 0U;
		g_mc.walking_active = false;
		g_mc.pickup_confidence = 0U;
		g_mc.in_hand_confidence = 0U;
		g_mc.look_confidence = 0U;
		g_mc.look_target_x = move_towards(g_mc.look_target_x, 0, 12);
		g_mc.look_target_y = move_towards(g_mc.look_target_y, 0, 12);
		reset_look_reference();
		publish_carry(k_uptime_get(), true);
		publish_look(k_uptime_get(), true);
		set_mode(MODE_IDLE, k_uptime_get());
		return;
	}

	err = motion_sensor_fetch_sample(&sample);
	if (err != 0) {
		LOG_WRN("Sample fetch failed (%d)", err);
		set_mode(MODE_IDLE, k_uptime_get());
		return;
	}

	normalise_sample(&sample);
	now_ms = sample.timestamp_ms;
	g_mc.last_motion_sample_ms = now_ms;

	prev_gx = g_mc.gravity_x;
	prev_gy = g_mc.gravity_y;
	prev_gz = g_mc.gravity_z;
	update_gravity(&sample);

	/* ── Idle polling: check for motion wake ───────────────────────── */

	if (idle_poll) {
		uint16_t idle_motion = (uint16_t)(
			abs16(sample.accel_mg_x - g_mc.gravity_x) +
			abs16(sample.accel_mg_y - g_mc.gravity_y) +
			abs16(sample.accel_mg_z - g_mc.gravity_z));
		uint16_t idle_smooth = (uint16_t)(
			abs16(sample.accel_mg_x - g_mc.last_sample.accel_mg_x) +
			abs16(sample.accel_mg_y - g_mc.last_sample.accel_mg_y) +
			abs16(sample.accel_mg_z - g_mc.last_sample.accel_mg_z));

		g_mc.last_sample = sample;

		if (idle_motion >= IDLE_WAKE_THRESHOLD_MG ||
		    idle_smooth >= IDLE_WAKE_SMOOTH_MG) {
			g_mc.last_motion_wake_ms = now_ms;
			g_mc.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
			(void)app_event_publish_with_timestamp(
				APP_EVENT_MOTION_WAKE, idle_motion, now_ms);
			set_mode(MODE_ACTIVE_WINDOW, g_mc.active_until_ms);
			return;
		}

		(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(IDLE_POLL_MS));
		return;
	}

	/* ── Active processing ─────────────────────────────────────────── */

	hw_steps = read_hw_steps(now_ms);

	lx = sample.accel_mg_x - g_mc.gravity_x;
	ly = sample.accel_mg_y - g_mc.gravity_y;
	lz = sample.accel_mg_z - g_mc.gravity_z;

	orient_delta = (uint16_t)(abs16(g_mc.gravity_x - prev_gx) +
				  abs16(g_mc.gravity_y - prev_gy) +
				  abs16(g_mc.gravity_z - prev_gz));
	orient_rate  = orient_delta;
	jerk_mg  = (uint16_t)(abs16(lx - g_mc.prev_linear_x) +
			      abs16(ly - g_mc.prev_linear_y) +
			      abs16(lz - g_mc.prev_linear_z));
	motion_mg = (uint16_t)(abs16(lx) + abs16(ly) + abs16(lz));
	smooth_mg = (uint16_t)(abs16(sample.accel_mg_x - g_mc.last_sample.accel_mg_x) +
			       abs16(sample.accel_mg_y - g_mc.last_sample.accel_mg_y) +
			       abs16(sample.accel_mg_z - g_mc.last_sample.accel_mg_z));
	gyro_sum = sample.gyro_valid ?
		(uint32_t)(abs16(sample.gyro_mdps_x) + abs16(sample.gyro_mdps_y) +
			   abs16(sample.gyro_mdps_z)) : 0U;

	/* Track last meaningful motion for soft mode transitions. */
	if (motion_mg >= 60U) {
		g_mc.last_active_motion_ms = now_ms;
	}

	/* Feature window. */
	memset(&frame, 0, sizeof(frame));
	frame.motion_mg = motion_mg;
	frame.smooth_motion_mg = smooth_mg;
	frame.jerk_mg = jerk_mg;
	frame.orientation_delta_mg = orient_delta;
	frame.orientation_rate_mg = orient_rate;
	frame.gyro_sum_mdps = gyro_sum;
	frame.timestamp_ms = now_ms;
	append_frame(&frame);
	summarise_features(&summary, now_ms);
	summary.cadence_confidence = compute_cadence_confidence(now_ms);

	g_mc.stability_confidence = summary.stability_confidence;
	g_mc.cadence_confidence   = summary.cadence_confidence;
	g_mc.chaos_confidence     = summary.chaos_confidence;

	rough = motion_mg >= 1200U || gyro_sum >= 80000U || summary.chaos_confidence >= 80U;

	/* Step detection. */
	if (!motion_sensor_get_capabilities()->backend_has_hw_step_counter) {
		step = detect_sw_step(lx, ly, lz, rough, now_ms);
	}
	update_walking(step || (hw_steps > 0U), &summary, now_ms);

	/* In-hand detection. */
	memset(&det_in, 0, sizeof(det_in));
	det_in.gravity_x           = g_mc.gravity_x;
	det_in.gravity_y           = g_mc.gravity_y;
	det_in.gravity_z           = g_mc.gravity_z;
	det_in.motion_mg           = motion_mg;
	det_in.smooth_motion_mg    = smooth_mg;
	det_in.jerk_mg             = jerk_mg;
	det_in.orientation_delta_mg = orient_delta;
	det_in.orientation_rate_mg  = orient_rate;
	det_in.gyro_sum_mdps       = gyro_sum;
	det_in.walking_confidence  = g_mc.walking_confidence;
	det_in.cadence_confidence  = summary.cadence_confidence;
	det_in.stability_confidence = summary.stability_confidence;
	det_in.chaos_confidence    = summary.chaos_confidence;
	det_in.walking_active      = g_mc.walking_active;
	det_in.rough_motion        = rough;
	det_in.motion_wake         = (now_ms - g_mc.last_motion_wake_ms) <= 1200LL;
	det_in.now_ms              = now_ms;

	in_hand_detector_process(&g_mc.in_hand_det, &det_in, &det_out);

	g_mc.pickup_confidence  = det_out.pickup_confidence;
	g_mc.in_hand_confidence = det_out.in_hand_confidence;
	g_mc.in_hand            = det_out.in_hand;
	if (det_out.state == IN_HAND_DETECTOR_SURFACE_STILL) {
		g_mc.last_still_ms = now_ms;
	}

	/* Shake detection. */
	check_shake(motion_mg, gyro_sum,
		    summary.stability_confidence, summary.chaos_confidence,
		    det_out.in_hand, det_out.in_hand_confidence, now_ms);

	/* Gaze. */
	emit_detector_events(&det_out, now_ms);
	update_look_reference(&det_out, &summary);
	update_look_target(&det_out, &summary, now_ms);

	/* Publish. */
	publish_carry(now_ms, det_out.pickup_candidate || det_out.picked_up ||
				det_out.in_hand_enter || det_out.in_hand_exit);
	publish_look(now_ms, false);

	g_mc.prev_linear_x = lx;
	g_mc.prev_linear_y = ly;
	g_mc.prev_linear_z = lz;
	g_mc.last_sample = sample;
	debug_log(now_ms);

	/* ── Mode decision ─────────────────────────────────────────────── */

	if (g_mc.walking_active) {
		set_mode(MODE_WALK_MAINTAIN, now_ms + 1000LL);
	} else if (g_mc.in_hand || g_mc.in_hand_confidence >= 40U) {
		set_mode(MODE_IN_HAND_TRACK,
			 now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS);
	} else if (now_ms < g_mc.active_until_ms) {
		set_mode(MODE_ACTIVE_WINDOW, g_mc.active_until_ms);
	} else if (motion_mg >= 60U &&
		   (now_ms - g_mc.last_active_motion_ms) < 2000LL) {
		/* Soft linger: subtle motion still present. */
		g_mc.active_until_ms = now_ms + 1500LL;
		set_mode(MODE_ACTIVE_WINDOW, g_mc.active_until_ms);
	} else {
		set_mode(MODE_IDLE, now_ms);
		return;
	}

	(void)k_work_reschedule(&g_mc.sample_work, K_MSEC(sample_period_ms()));
}

/* ── ISR handler ──────────────────────────────────────────────────────── */

static void sensor_event_handler(uint32_t events, void *user_data)
{
	int64_t now_ms = k_uptime_get();

	ARG_UNUSED(user_data);

	if (!g_mc.enabled || g_mc.battery_critical) {
		return;
	}

	if ((now_ms - g_mc.last_motion_wake_ms) >= 750LL) {
		(void)app_event_publish_with_timestamp(
			APP_EVENT_MOTION_WAKE, (int32_t)events, now_ms);
	}
	g_mc.last_motion_wake_ms = now_ms;
	g_mc.active_until_ms = now_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
	set_mode(MODE_ACTIVE_WINDOW, g_mc.active_until_ms);
}

/* ── Public API ───────────────────────────────────────────────────────── */

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
		LOG_INF("Motion: polling fallback (no interrupt)");
	}

	set_mode(MODE_IDLE, now_ms);
	return 0;
}

void motion_classifier_on_event(const struct app_event *event, const struct pet_state *state)
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
		break;
	case APP_EVENT_BATTERY_LOW:
		g_mc.battery_low = true;
		break;
	case APP_EVENT_BATTERY_CRITICAL:
		g_mc.battery_critical = true;
		reset_look_reference();
		break;
	case APP_EVENT_CHARGER_CONNECTED:
		g_mc.charging = true;
		break;
	case APP_EVENT_CHARGER_DISCONNECTED:
		g_mc.charging = false;
		break;
	case APP_EVENT_WALKING_START:
		g_mc.walking_active = true;
		g_mc.walking_confidence = (event->param > 0) ?
			clamp_u8(event->param) : CONFIG_KERFUR_MOTION_WALK_START_THRESHOLD;
		g_mc.active_until_ms = event->timestamp_ms + 1500LL;
		g_mc.suppress_look_until_ms = max64(
			g_mc.suppress_look_until_ms,
			event->timestamp_ms + LOOK_SUPPRESS_AFTER_WALK_MS);
		break;
	case APP_EVENT_WALKING_STOP:
		g_mc.walking_active = false;
		g_mc.walking_confidence = clamp_u8(event->param);
		break;
	case APP_EVENT_CARRY_STATE_UPDATE:
		g_mc.in_hand = event->payload.carry_state.in_hand;
		g_mc.pickup_confidence = event->payload.carry_state.pickup_confidence;
		g_mc.in_hand_confidence = event->payload.carry_state.in_hand_confidence;
		g_mc.walking_confidence = event->payload.carry_state.walking_confidence;
		if (!g_mc.in_hand) {
			reset_look_reference();
		}
		break;
	case APP_EVENT_PICKED_UP:
		g_mc.last_pickup_ms = event->timestamp_ms;
		g_mc.active_until_ms = event->timestamp_ms + CONFIG_KERFUR_MOTION_ACTIVE_WINDOW_MS;
		break;
	case APP_EVENT_IN_HAND_ENTER:
		g_mc.last_in_hand_ms = event->timestamp_ms;
		g_mc.in_hand = true;
		break;
	case APP_EVENT_IN_HAND_EXIT:
		g_mc.in_hand = false;
		reset_look_reference();
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
