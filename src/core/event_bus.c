#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/app_event.h"
#include "core/event_bus.h"

LOG_MODULE_REGISTER(event_bus, CONFIG_LOG_DEFAULT_LEVEL);

#define APP_EVENT_QUEUE_LEN 128

K_MSGQ_DEFINE(g_event_queue, sizeof(struct app_event), APP_EVENT_QUEUE_LEN, 4);

const char *app_event_type_str(enum app_event_type type);

static bool event_is_periodic_tick(enum app_event_type type)
{
	return (type == APP_EVENT_TICK_100MS) ||
	       (type == APP_EVENT_TICK_1S) ||
	       (type == APP_EVENT_TICK_10S) ||
	       (type == APP_EVENT_TICK_60S);
}

static const char *const g_event_names[APP_EVENT_COUNT] = {
	[APP_EVENT_TICK_100MS] = "TICK_100MS",
	[APP_EVENT_TICK_1S] = "TICK_1S",
	[APP_EVENT_TICK_10S] = "TICK_10S",
	[APP_EVENT_TICK_60S] = "TICK_60S",
	[APP_EVENT_USER_TAP] = "USER_TAP",
	[APP_EVENT_USER_PET_SOFT] = "USER_PET_SOFT",
	[APP_EVENT_USER_PET_LONG] = "USER_PET_LONG",
	[APP_EVENT_USER_HOLD] = "USER_HOLD",
	[APP_EVENT_MOTION_WAKE] = "MOTION_WAKE",
	[APP_EVENT_WALKING_START] = "WALKING_START",
	[APP_EVENT_WALKING_STOP] = "WALKING_STOP",
	[APP_EVENT_STEP_BATCH] = "STEP_BATCH",
	[APP_EVENT_PICKUP_CANDIDATE] = "PICKUP_CANDIDATE",
	[APP_EVENT_PICKED_UP] = "PICKED_UP",
	[APP_EVENT_IN_HAND_ENTER] = "IN_HAND_ENTER",
	[APP_EVENT_IN_HAND_EXIT] = "IN_HAND_EXIT",
	[APP_EVENT_SHAKE_LIGHT] = "SHAKE_LIGHT",
	[APP_EVENT_SHAKE_PLAY] = "SHAKE_PLAY",
	[APP_EVENT_SHAKE_ROUGH] = "SHAKE_ROUGH",
	[APP_EVENT_FLIP_FACE_DOWN] = "FLIP_FACE_DOWN",
	[APP_EVENT_IMPACT] = "IMPACT",
	[APP_EVENT_PHONE_CONNECTED] = "PHONE_CONNECTED",
	[APP_EVENT_PHONE_DISCONNECTED] = "PHONE_DISCONNECTED",
	[APP_EVENT_PHONE_NOTIFICATION_SINGLE] = "PHONE_NOTIFICATION_SINGLE",
	[APP_EVENT_PHONE_NOTIFICATION_BURST] = "PHONE_NOTIFICATION_BURST",
	[APP_EVENT_APP_SESSION_START] = "APP_SESSION_START",
	[APP_EVENT_APP_SESSION_END] = "APP_SESSION_END",
	[APP_EVENT_TIME_SYNC] = "TIME_SYNC",
	[APP_EVENT_CHARGER_CONNECTED] = "CHARGER_CONNECTED",
	[APP_EVENT_CHARGER_DISCONNECTED] = "CHARGER_DISCONNECTED",
	[APP_EVENT_BATTERY_LOW] = "BATTERY_LOW",
	[APP_EVENT_BATTERY_CRITICAL] = "BATTERY_CRITICAL",
	[APP_EVENT_SELF_WAKE_TIMER] = "SELF_WAKE_TIMER",
	[APP_EVENT_IDLE_TIMEOUT] = "IDLE_TIMEOUT",
	[APP_EVENT_WAKE] = "WAKE",
	[APP_EVENT_SLEEP_REQUEST] = "SLEEP_REQUEST",
	[APP_EVENT_DISPLAY_FOREGROUND_TIMEOUT] = "DISPLAY_FOREGROUND_TIMEOUT",
	[APP_EVENT_DISPLAY_AMBIENT_TIMEOUT] = "DISPLAY_AMBIENT_TIMEOUT",
	[APP_EVENT_LOOK_TARGET_UPDATE] = "LOOK_TARGET_UPDATE",
	[APP_EVENT_CARRY_STATE_UPDATE] = "CARRY_STATE_UPDATE",
	[APP_EVENT_CARRY_CONTEXT_CHANGED] = "CARRY_CONTEXT_CHANGED",
	[APP_EVENT_WORN_STYLE_SET] = "WORN_STYLE_SET",
	[APP_EVENT_BATTERY_PERCENT_UPDATE] = "BATTERY_PERCENT_UPDATE",
	[APP_EVENT_FACE_FORCE_EXPRESSION] = "FACE_FORCE_EXPRESSION",
	[APP_EVENT_FACE_CLEAR_FORCED_EXPRESSION] = "FACE_CLEAR_FORCED_EXPRESSION",
	[APP_EVENT_FACE_TRIGGER_REACTION] = "FACE_TRIGGER_REACTION",
	[APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG] = "FACE_SET_DYNAMIC_PUPILS_DEBUG",
	[APP_EVENT_FACE_DEBUG_DUMP] = "FACE_DEBUG_DUMP",
	[APP_EVENT_PERSONALITY_SET] = "PERSONALITY_SET",
	[APP_EVENT_PEER_SEEN] = "PEER_SEEN",
	[APP_EVENT_PEER_CHECKING] = "PEER_CHECKING",
	[APP_EVENT_PEER_NEAR] = "PEER_NEAR",
	[APP_EVENT_PEER_LOST] = "PEER_LOST",
	[APP_EVENT_ENCOUNTER_START] = "ENCOUNTER_START",
	[APP_EVENT_ENCOUNTER_END] = "ENCOUNTER_END",
	[APP_EVENT_PEER_PLAY_INVITE] = "PEER_PLAY_INVITE",
	[APP_EVENT_PEER_PLAY_ACK] = "PEER_PLAY_ACK",
	[APP_EVENT_PEER_GREET] = "PEER_GREET",
	[APP_EVENT_PEER_GREET_ACK] = "PEER_GREET_ACK",
};

static void log_event_payload(const struct app_event *event)
{
	switch (event->type) {
	case APP_EVENT_LOOK_TARGET_UPDATE:
		LOG_INF("Event pub: %s x=%d y=%d conf=%u",
			app_event_type_str(event->type),
			event->payload.look_target.x,
			event->payload.look_target.y,
			event->payload.look_target.confidence);
		break;
	case APP_EVENT_CARRY_STATE_UPDATE:
		LOG_INF("Event pub: %s in_hand=%d pickup=%u in_hand_conf=%u walk_conf=%u",
			app_event_type_str(event->type),
			event->payload.carry_state.in_hand ? 1 : 0,
			event->payload.carry_state.pickup_confidence,
			event->payload.carry_state.in_hand_confidence,
			event->payload.carry_state.walking_confidence);
		break;
	case APP_EVENT_STEP_BATCH:
		LOG_INF("Event pub: %s steps=%d hw_counter=%u walk_conf=%u hw=%d",
			app_event_type_str(event->type),
			event->param,
			event->payload.step_batch.hw_counter,
			event->payload.step_batch.walking_confidence,
			event->payload.step_batch.from_hw_counter ? 1 : 0);
		break;
	case APP_EVENT_BATTERY_PERCENT_UPDATE:
		LOG_INF("Event pub: %s percent=%d known=%d",
			app_event_type_str(event->type),
			event->payload.battery_percent.percent,
			event->payload.battery_percent.known ? 1 : 0);
		break;
	default:
		LOG_INF("Event pub: %s param=%d", app_event_type_str(event->type), event->param);
		break;
	}
}

static int publish_event(const struct app_event *event)
{
	int err;

	err = k_msgq_put(&g_event_queue, event, K_NO_WAIT);
	if (err != 0) {
		if (!event_is_periodic_tick(event->type)) {
			LOG_WRN("Event drop: %s param=%d ts=%lld (err=%d)",
				app_event_type_str(event->type), event->param,
				(long long)event->timestamp_ms, err);
		}
	} else if (IS_ENABLED(CONFIG_KERFUR_TRACE_EVENTS) &&
		   (event->type != APP_EVENT_TICK_100MS) &&
		   (event->type != APP_EVENT_TICK_1S)) {
		log_event_payload(event);
	}

	return err;
}

int app_event_bus_init(void)
{
	k_msgq_purge(&g_event_queue);
	LOG_INF("Event bus initialized (queue=%d)", APP_EVENT_QUEUE_LEN);
	return 0;
}

int app_event_publish_with_timestamp(enum app_event_type type, int32_t param, int64_t timestamp_ms)
{
	struct app_event event = {0};

	event.type = type;
	event.timestamp_ms = timestamp_ms;
	event.param = param;
	return publish_event(&event);
}

int app_event_publish(enum app_event_type type, int32_t param)
{
	return app_event_publish_with_timestamp(type, param, k_uptime_get());
}

int app_event_publish_step_batch_with_timestamp(int32_t steps, uint16_t hw_counter,
						uint8_t walking_confidence,
						bool from_hw_counter,
						int64_t timestamp_ms)
{
	struct app_event event = {
		.type = APP_EVENT_STEP_BATCH,
		.timestamp_ms = timestamp_ms,
		.param = steps,
		.payload = {
			.step_batch = {
				.hw_counter = hw_counter,
				.walking_confidence = walking_confidence,
				.from_hw_counter = from_hw_counter,
			},
		},
	};

	return publish_event(&event);
}

int app_event_publish_step_batch(int32_t steps, uint16_t hw_counter, uint8_t walking_confidence,
				 bool from_hw_counter)
{
	return app_event_publish_step_batch_with_timestamp(steps, hw_counter, walking_confidence,
							       from_hw_counter, k_uptime_get());
}

int app_event_publish_look_target_with_timestamp(int16_t x, int16_t y, uint8_t confidence,
						 int64_t timestamp_ms)
{
	struct app_event event = {
		.type = APP_EVENT_LOOK_TARGET_UPDATE,
		.timestamp_ms = timestamp_ms,
		.param = confidence,
		.payload = {
			.look_target = {
				.x = x,
				.y = y,
				.confidence = confidence,
			},
		},
	};

	return publish_event(&event);
}

int app_event_publish_look_target(int16_t x, int16_t y, uint8_t confidence)
{
	return app_event_publish_look_target_with_timestamp(x, y, confidence, k_uptime_get());
}

int app_event_publish_carry_with_timestamp(enum app_event_type type,
					   const struct app_event_carry_state *carry,
					   int64_t timestamp_ms)
{
	struct app_event event = {
		.type = type,
		.timestamp_ms = timestamp_ms,
	};

	if (carry != NULL) {
		event.payload.carry_state = *carry;
		event.param = (type == APP_EVENT_CARRY_CONTEXT_CHANGED) ?
			      (int32_t)carry->carry_context :
			      (carry->in_hand ? 1 : 0);
	}

	return publish_event(&event);
}

int app_event_publish_carry_state_with_timestamp(bool in_hand, uint8_t pickup_confidence,
						 uint8_t in_hand_confidence,
						 uint8_t walking_confidence,
						 int64_t timestamp_ms)
{
	struct app_event_carry_state carry = {
		.in_hand = in_hand,
		.pickup_confidence = pickup_confidence,
		.in_hand_confidence = in_hand_confidence,
		.walking_confidence = walking_confidence,
		.carry_context = 0U,            /* PET_CARRY_UNKNOWN */
		.carry_context_confidence = 0U,
	};

	return app_event_publish_carry_with_timestamp(APP_EVENT_CARRY_STATE_UPDATE,
						      &carry, timestamp_ms);
}

int app_event_publish_carry_state(bool in_hand, uint8_t pickup_confidence,
				 uint8_t in_hand_confidence, uint8_t walking_confidence)
{
	return app_event_publish_carry_state_with_timestamp(in_hand, pickup_confidence,
							    in_hand_confidence, walking_confidence,
							    k_uptime_get());
}

int app_event_publish_battery_percent_with_timestamp(int8_t percent, bool known,
						     int64_t timestamp_ms)
{
	struct app_event event = {
		.type = APP_EVENT_BATTERY_PERCENT_UPDATE,
		.timestamp_ms = timestamp_ms,
		.param = known ? percent : -1,
		.payload = {
			.battery_percent = {
				.percent = percent,
				.known = known,
			},
		},
	};

	return publish_event(&event);
}

int app_event_publish_battery_percent(int8_t percent, bool known)
{
	return app_event_publish_battery_percent_with_timestamp(percent, known, k_uptime_get());
}

int app_event_publish_peer_with_timestamp(enum app_event_type type,
					  const struct app_event_peer *peer,
					  int64_t timestamp_ms)
{
	struct app_event event = {
		.type = type,
		.timestamp_ms = timestamp_ms,
		.param = (peer != NULL) ? (int32_t)peer->ephemeral_id : 0,
	};

	if (peer != NULL) {
		event.payload.peer = *peer;
	}

	return publish_event(&event);
}

int app_event_publish_peer(enum app_event_type type, const struct app_event_peer *peer)
{
	return app_event_publish_peer_with_timestamp(type, peer, k_uptime_get());
}

bool app_event_wait(struct app_event *event, k_timeout_t timeout)
{
	return k_msgq_get(&g_event_queue, event, timeout) == 0;
}

const char *app_event_type_str(enum app_event_type type)
{
	if ((type < 0) || (type >= APP_EVENT_COUNT) || (g_event_names[type] == NULL)) {
		return "UNKNOWN";
	}

	return g_event_names[type];
}
