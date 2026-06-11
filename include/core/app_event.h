#ifndef KERFUR_APP_EVENT_H_
#define KERFUR_APP_EVENT_H_

#include <stdbool.h>
#include <stdint.h>

enum app_event_type {
	APP_EVENT_TICK_100MS = 0,
	APP_EVENT_TICK_1S,
	APP_EVENT_TICK_10S,
	APP_EVENT_TICK_60S,

	APP_EVENT_USER_TAP,
	APP_EVENT_USER_PET_SOFT,
	APP_EVENT_USER_PET_LONG,
	APP_EVENT_USER_HOLD,

	APP_EVENT_MOTION_WAKE,
	APP_EVENT_WALKING_START,
	APP_EVENT_WALKING_STOP,
	APP_EVENT_STEP_BATCH,
	APP_EVENT_PICKUP_CANDIDATE,
	APP_EVENT_PICKED_UP,
	APP_EVENT_IN_HAND_ENTER,
	APP_EVENT_IN_HAND_EXIT,
	APP_EVENT_SHAKE_LIGHT,
	APP_EVENT_SHAKE_PLAY,
	APP_EVENT_SHAKE_ROUGH,
	APP_EVENT_FLIP_FACE_DOWN,
	APP_EVENT_IMPACT,

	APP_EVENT_PHONE_CONNECTED,
	APP_EVENT_PHONE_DISCONNECTED,
	APP_EVENT_PHONE_NOTIFICATION_SINGLE,
	APP_EVENT_PHONE_NOTIFICATION_BURST,
	APP_EVENT_APP_SESSION_START,
	APP_EVENT_APP_SESSION_END,
	APP_EVENT_TIME_SYNC,

	APP_EVENT_CHARGER_CONNECTED,
	APP_EVENT_CHARGER_DISCONNECTED,
	APP_EVENT_BATTERY_LOW,
	APP_EVENT_BATTERY_CRITICAL,

	APP_EVENT_SELF_WAKE_TIMER,
	APP_EVENT_IDLE_TIMEOUT,
	APP_EVENT_WAKE,
	APP_EVENT_SLEEP_REQUEST,
	APP_EVENT_DISPLAY_FOREGROUND_TIMEOUT,
	APP_EVENT_DISPLAY_AMBIENT_TIMEOUT,
	APP_EVENT_LOOK_TARGET_UPDATE,
	APP_EVENT_CARRY_STATE_UPDATE,
	APP_EVENT_BATTERY_PERCENT_UPDATE,
	APP_EVENT_FACE_FORCE_EXPRESSION,
	APP_EVENT_FACE_CLEAR_FORCED_EXPRESSION,
	APP_EVENT_FACE_TRIGGER_REACTION,
	APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG,
	APP_EVENT_FACE_DEBUG_DUMP,
	APP_EVENT_PERSONALITY_SET,

	APP_EVENT_PEER_SEEN,
	APP_EVENT_PEER_CHECKING,
	APP_EVENT_PEER_NEAR,
	APP_EVENT_PEER_LOST,
	APP_EVENT_ENCOUNTER_START,
	APP_EVENT_ENCOUNTER_END,
	APP_EVENT_PEER_PLAY_INVITE,
	APP_EVENT_PEER_PLAY_ACK,

	APP_EVENT_COUNT
};

struct app_event_look_target {
	int16_t x;
	int16_t y;
	uint8_t confidence;
};

struct app_event_carry_state {
	bool in_hand;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t walking_confidence;
};

struct app_event_step_batch {
	uint16_t hw_counter;
	uint8_t walking_confidence;
	bool from_hw_counter;
};

struct app_event_battery_percent {
	int8_t percent;
	bool known;
};

struct app_event_peer {
	uint32_t ephemeral_id;
	uint32_t encounter_id;       /* 0 if not applicable */
	int8_t   rssi;
	bool     is_friend;
	uint8_t  character_id;
	uint8_t  mode_summary;
	uint8_t  expression_summary;
	uint8_t  status_flags;
	int32_t  duration_s;         /* used by ENCOUNTER_END */
};

struct app_event_payload {
	struct app_event_look_target look_target;
	struct app_event_carry_state carry_state;
	struct app_event_step_batch step_batch;
	struct app_event_battery_percent battery_percent;
	struct app_event_peer peer;
};

struct app_event {
	enum app_event_type type;
	int64_t timestamp_ms;
	int32_t param;
	struct app_event_payload payload;
};

const char *app_event_type_str(enum app_event_type type);

/* Backward compatibility aliases for existing mock/debug plumbing. */
#define APP_EVENT_USER_BUTTON_PRESS APP_EVENT_USER_TAP
#define APP_EVENT_MOCK_PET APP_EVENT_USER_PET_SOFT
#define APP_EVENT_MOCK_SHAKE APP_EVENT_SHAKE_LIGHT
#define APP_EVENT_MOCK_NOTIFICATION APP_EVENT_PHONE_NOTIFICATION_BURST
#define APP_EVENT_PHONE_NOTIFICATION APP_EVENT_PHONE_NOTIFICATION_SINGLE
#define APP_EVENT_BLE_CONNECTED APP_EVENT_PHONE_CONNECTED
#define APP_EVENT_BLE_DISCONNECTED APP_EVENT_PHONE_DISCONNECTED
#define APP_EVENT_LOOK_VECTOR_UPDATE APP_EVENT_LOOK_TARGET_UPDATE

#endif /* KERFUR_APP_EVENT_H_ */
