#ifndef KERFUR_MOTION_SENSOR_H_
#define KERFUR_MOTION_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

struct motion_sensor_capabilities {
	bool backend_ready;
	bool backend_has_hw_step_counter;
	bool backend_has_significant_motion;
	bool backend_has_tilt;
	bool backend_has_stationary_motion;
	bool backend_has_fsm;
	bool backend_has_mlc;
};

struct motion_sensor_sample {
	int16_t accel_mg_x;
	int16_t accel_mg_y;
	int16_t accel_mg_z;
	int16_t gyro_mdps_x;
	int16_t gyro_mdps_y;
	int16_t gyro_mdps_z;
	bool gyro_valid;
	int64_t timestamp_ms;
};

enum motion_sensor_mode {
	MOTION_SENSOR_MODE_DISABLED = 0,
	MOTION_SENSOR_MODE_IDLE,
	MOTION_SENSOR_MODE_ACTIVE,
	MOTION_SENSOR_MODE_WALK_MAINTAIN,
	MOTION_SENSOR_MODE_IN_HAND_TRACK,
};

typedef void (*motion_sensor_event_handler_t)(uint32_t events, void *user_data);

int motion_sensor_init(void);
bool motion_sensor_is_ready(void);
enum motion_sensor_mode motion_sensor_get_mode(void);
const struct motion_sensor_capabilities *motion_sensor_get_capabilities(void);
int motion_sensor_set_event_handler(motion_sensor_event_handler_t handler, void *user_data);
int motion_sensor_set_mode(enum motion_sensor_mode mode);
int motion_sensor_fetch_sample(struct motion_sensor_sample *out_sample);
int motion_sensor_read_hw_step_counter(uint16_t *out_counter);

#endif /* KERFUR_MOTION_SENSOR_H_ */
