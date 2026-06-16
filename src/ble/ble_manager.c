
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/assigned_numbers.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "ble/ble_manager.h"
#include "core/event_bus.h"
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
#include "nearby/kerfur_nearby.h"
#endif

LOG_MODULE_REGISTER(ble_manager, CONFIG_LOG_DEFAULT_LEVEL);

#define BT_UUID_KERFUR_DEBUG_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x4a7b0001, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)
#define BT_UUID_KERFUR_EVENT_INJECT_VAL \
	BT_UUID_128_ENCODE(0x4a7b0002, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)

#define BT_UUID_KERFUR_COMPANION_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x4a7b1001, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)
#define BT_UUID_KERFUR_COMPANION_RX_VAL \
	BT_UUID_128_ENCODE(0x4a7b1002, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)
#define BT_UUID_KERFUR_COMPANION_TX_VAL \
	BT_UUID_128_ENCODE(0x4a7b1003, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)

#define BT_UUID_ANCS_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x7905f431, 0xb5ce, 0x4e99, 0xa40f, 0x4b1e122d00d0)
#define BT_UUID_ANCS_NOTIFICATION_SOURCE_VAL \
	BT_UUID_128_ENCODE(0x9fbf120d, 0x6301, 0x42d9, 0x8c58, 0x25e699a21dbd)
#define BT_UUID_ANCS_CONTROL_POINT_VAL \
	BT_UUID_128_ENCODE(0x69d1d8f3, 0x45e1, 0x49a8, 0x9821, 0x9bbdfdaad9d9)
#define BT_UUID_ANCS_DATA_SOURCE_VAL \
	BT_UUID_128_ENCODE(0x22eac6e9, 0x24d6, 0x4bb5, 0xbe44, 0xb36ace7c7bfb)

#define ANCS_EVENT_ID_NOTIFICATION_ADDED 0U
#define ANCS_NOTIFICATION_SOURCE_LEN 8U

#define ANS_NEW_ALERT_MIN_LEN 2U
#define ANS_UNREAD_ALERT_MIN_LEN 2U

#define KERFUR_GB_PROTO_VERSION 1U
#define KERFUR_GB_OP_PING 0x01U
#define KERFUR_GB_OP_ANDROID_NOTIFICATION 0x10U
#define KERFUR_GB_EVT_PONG 0x81U
#define KERFUR_GB_EVT_NOTIFICATION_ACK 0x82U
#define KERFUR_GB_EVT_TEST_NOTIFICATION 0x90U
#define KERFUR_GB_EVT_ERROR 0xE0U
#define KERFUR_GB_ERR_UNSUPPORTED_OPCODE 1U

#define KERFUR_ADV_MAX_NAME_LEN 8U
#define KERFUR_ADV_FAST_TIMEOUT K_SECONDS(45)
#define KERFUR_CONN_PARAM_RETRY_DELAY K_SECONDS(2)
#define KERFUR_ADV_RESTART_INITIAL_DELAY K_MSEC(120)
#define KERFUR_ADV_RESTART_RETRY_DELAY K_MSEC(300)
#define KERFUR_ADV_RESTART_MAX_RETRIES 8U
/* Min spacing between social-handshake-triggered beacon refreshes (adv restarts)
 * so a flurry of greet/ack emits coalesces instead of churning the radio. */
#define KERFUR_BEACON_REFRESH_MIN_MS 1500
#define KERFUR_NOTIFY_DISCOVERY_RETRY_DELAY K_SECONDS(2)
#define KERFUR_NOTIFY_DISCOVERY_MAX_RETRIES 5U

/* Re-pair backoff: when a phone keeps a bond we no longer have, it auto-
 * reconnects and re-encrypts with a key we reject, looping ~1/s. Escalate the
 * re-advertise delay after each key-missing failure to break the loop and save
 * power until the user forgets+re-pairs; a successful pairing resets it. */
#define KERFUR_REPAIR_BACKOFF_STEP_MS 4000
#define KERFUR_REPAIR_BACKOFF_MAX_MS 30000
#define KERFUR_REPAIR_STREAK_RESET_MS 60000

#define KERFUR_COMPANION_MAX_PACKET_LEN 20U
#define KERFUR_ADV_UUID16_MAX_LEN 14U

#define KERFUR_ALERT_LEVEL_NO_ALERT 0U
#define KERFUR_ALERT_LEVEL_MILD_ALERT 1U
#define KERFUR_ALERT_LEVEL_HIGH_ALERT 2U

#define KERFUR_RSC_MEAS_ATTR_INDEX 2U
#define KERFUR_RSC_FLAG_RUNNING BIT(2)
#define KERFUR_RSC_FEATURES KERFUR_RSC_FLAG_RUNNING
#define KERFUR_RSC_SENSOR_LOCATION 0U

#define KERFUR_COMPANION_TX_ATTR_INDEX 4U

#define KERFUR_FACE_DEBUG_MAGIC 0xF0U
#define KERFUR_FACE_DEBUG_VERSION 1U
#define KERFUR_FACE_DEBUG_OP_LOOK_TARGET 0x01U
#define KERFUR_FACE_DEBUG_OP_CARRY_STATE 0x02U
#define KERFUR_FACE_DEBUG_OP_BATTERY_PERCENT 0x03U
#define KERFUR_FACE_DEBUG_OP_PICKUP_CANDIDATE 0x04U
#define KERFUR_FACE_DEBUG_OP_PICKED_UP 0x05U
#define KERFUR_FACE_DEBUG_OP_IN_HAND_ENTER 0x06U
#define KERFUR_FACE_DEBUG_OP_IN_HAND_EXIT 0x07U
#define KERFUR_FACE_DEBUG_OP_DYNAMIC_PUPILS 0x08U

struct ancs_client_state {
	struct bt_conn *conn;
	uint16_t service_start_handle;
	uint16_t service_end_handle;
	uint16_t notif_source_handle;
	uint16_t control_point_handle;
	uint16_t data_source_handle;
	bool discovery_in_progress;
	bool notif_subscribed;
	struct bt_gatt_discover_params service_disc_params;
	struct bt_gatt_discover_params char_disc_params;
	struct bt_gatt_discover_params ccc_disc_params;
	struct bt_gatt_subscribe_params notif_sub_params;
};

struct ans_client_state {
	struct bt_conn *conn;
	uint16_t service_start_handle;
	uint16_t service_end_handle;
	uint16_t new_alert_handle;
	uint16_t unread_alert_handle;
	bool discovery_in_progress;
	bool new_alert_subscribed;
	bool unread_alert_subscribed;
	struct bt_gatt_discover_params service_disc_params;
	struct bt_gatt_discover_params char_disc_params;
	struct bt_gatt_discover_params new_alert_ccc_disc_params;
	struct bt_gatt_discover_params unread_alert_ccc_disc_params;
	struct bt_gatt_subscribe_params new_alert_sub_params;
	struct bt_gatt_subscribe_params unread_alert_sub_params;
};

enum ble_adv_phase {
	BLE_ADV_PHASE_IDLE = 0,
	BLE_ADV_PHASE_FAST,
	BLE_ADV_PHASE_SLOW,
};

static struct bt_uuid_128 g_kerfur_debug_service_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_DEBUG_SERVICE_VAL);
static struct bt_uuid_128 g_kerfur_event_inject_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_EVENT_INJECT_VAL);

static struct bt_uuid_128 g_ancs_service_uuid = BT_UUID_INIT_128(BT_UUID_ANCS_SERVICE_VAL);
static struct bt_uuid_128 g_ancs_notif_source_uuid = BT_UUID_INIT_128(BT_UUID_ANCS_NOTIFICATION_SOURCE_VAL);
static struct bt_uuid_128 g_ancs_control_point_uuid = BT_UUID_INIT_128(BT_UUID_ANCS_CONTROL_POINT_VAL);
static struct bt_uuid_128 g_ancs_data_source_uuid = BT_UUID_INIT_128(BT_UUID_ANCS_DATA_SOURCE_VAL);

static struct bt_uuid_16 g_ans_service_uuid = BT_UUID_INIT_16(BT_UUID_ANS_VAL);
static struct bt_uuid_16 g_ans_new_alert_uuid = BT_UUID_INIT_16(BT_UUID_GATT_NALRT_VAL);
static struct bt_uuid_16 g_ans_unread_alert_uuid = BT_UUID_INIT_16(BT_UUID_GATT_UALRTS_VAL);

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
static struct bt_uuid_128 g_kerfur_companion_service_uuid =
	BT_UUID_INIT_128(BT_UUID_KERFUR_COMPANION_SERVICE_VAL);
static struct bt_uuid_128 g_kerfur_companion_rx_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_COMPANION_RX_VAL);
static struct bt_uuid_128 g_kerfur_companion_tx_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_COMPANION_TX_VAL);
#endif

static const struct bt_le_adv_param g_adv_param_fast =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
			     BT_GAP_ADV_FAST_INT_MIN_1, BT_GAP_ADV_FAST_INT_MAX_1, NULL);
static const struct bt_le_adv_param g_adv_param_slow =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
			     BT_GAP_ADV_SLOW_INT_MIN, BT_GAP_ADV_SLOW_INT_MAX, NULL);

static const struct bt_le_conn_param *g_low_power_conn_param =
	BT_LE_CONN_PARAM(36, 60, 6, 420);

static const uint8_t g_adv_flags[] = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
static const uint8_t g_ancs_solicit_uuid[] = { BT_UUID_ANCS_SERVICE_VAL };
static const uint8_t g_adv_appearance_keyring[] = {
	BT_BYTES_LIST_LE16(BT_APPEARANCE_GENERIC_KEYRING),
};

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
static const uint8_t g_kerfur_companion_uuid_ad[] = { BT_UUID_KERFUR_COMPANION_SERVICE_VAL };
#endif

static struct ancs_client_state g_ancs;
static struct ans_client_state g_ans;

static struct bt_conn *g_active_conn;

static enum ble_adv_phase g_adv_phase = BLE_ADV_PHASE_IDLE;
static struct k_work_delayable g_adv_phase_work;
static struct k_work_delayable g_conn_param_work;
static struct k_work_delayable g_notify_discovery_work;
static struct k_work_delayable g_adv_restart_work;
static uint8_t g_notify_discovery_retries;
static uint8_t g_adv_restart_retries;
#if defined(CONFIG_BT_SMP)
static uint8_t g_repair_fail_streak;
static int64_t g_repair_last_fail_ms;
#endif
/* One-shot: re-advertise delay (ms) applied by the next disconnect; 0 = normal.
 * Set by the security-failure path to space out re-pair attempts. */
static int64_t g_repair_adv_delay_ms;

static struct bt_data g_adv_data[3];
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
static struct bt_data g_scan_rsp_data[5];
#else
static struct bt_data g_scan_rsp_data[3];
#endif
static size_t g_adv_data_len;
static size_t g_scan_rsp_data_len;
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
static uint8_t g_kerfur_beacon_buf[sizeof(struct kerfur_beacon_v1)];
static size_t g_kerfur_beacon_len;
static struct k_work_delayable g_kerfur_rotate_work;
#endif
static uint8_t g_adv_name[KERFUR_ADV_MAX_NAME_LEN + 1U];
static uint8_t g_adv_uuid16_visible[KERFUR_ADV_UUID16_MAX_LEN];
static size_t g_adv_uuid16_visible_len;
#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
static bool g_companion_notify_enabled;
static uint8_t g_companion_last_tx[KERFUR_COMPANION_MAX_PACKET_LEN];
static uint8_t g_companion_last_tx_len;
#endif

#if defined(CONFIG_KERFUR_ENABLE_RSC_SERVICE) && CONFIG_KERFUR_ENABLE_RSC_SERVICE
static bool g_rsc_notify_enabled;
static uint8_t g_rsc_measurement[4];
static uint16_t g_rsc_feature = KERFUR_RSC_FEATURES;
static uint8_t g_rsc_sensor_location = KERFUR_RSC_SENSOR_LOCATION;
#endif
#if defined(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) && CONFIG_KERFUR_ENABLE_KEYRING_PROFILE
static uint8_t g_lls_alert_level;
#endif

static uint8_t ancs_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params);
static int ancs_start_discovery(struct bt_conn *conn);

static uint8_t ans_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       struct bt_gatt_discover_params *params);
static int ans_start_discovery(struct bt_conn *conn);

static int phone_notification_discovery_start(struct bt_conn *conn);
static void request_phone_notification_discovery(struct bt_conn *conn);

static void build_advertising_payloads(void);
static int ble_start_advertising(void);
static int ble_set_advertising_phase(enum ble_adv_phase phase, bool restart);

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
static int companion_send_packet(const uint8_t *payload, uint8_t len);
#endif

static void reset_adv_timers(void)
{
	(void)k_work_cancel_delayable(&g_adv_phase_work);
	if (g_adv_phase == BLE_ADV_PHASE_FAST) {
		(void)k_work_schedule(&g_adv_phase_work, KERFUR_ADV_FAST_TIMEOUT);
	}
}

static size_t build_visible_uuid16_payload(uint8_t *dst, size_t dst_len)
{
	size_t used = 0U;

	if ((dst == NULL) || (dst_len < 4U)) {
		return 0U;
	}

#if defined(CONFIG_BT_BAS) && CONFIG_BT_BAS
	sys_put_le16(BT_UUID_BAS_VAL, &dst[used]);
	used += 2U;
#endif
#if defined(CONFIG_BT_DIS) && CONFIG_BT_DIS
	sys_put_le16(BT_UUID_DIS_VAL, &dst[used]);
	used += 2U;
#endif

#if defined(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) && CONFIG_KERFUR_ENABLE_KEYRING_PROFILE
	if ((used + 2U) <= dst_len) {
		sys_put_le16(BT_UUID_IAS_VAL, &dst[used]);
		used += 2U;
	}
	if ((used + 2U) <= dst_len) {
		sys_put_le16(BT_UUID_LLS_VAL, &dst[used]);
		used += 2U;
	}
	if ((used + 2U) <= dst_len) {
		sys_put_le16(BT_UUID_TPS_VAL, &dst[used]);
		used += 2U;
	}
#endif

#if defined(CONFIG_KERFUR_ENABLE_RSC_SERVICE) && CONFIG_KERFUR_ENABLE_RSC_SERVICE
	if ((used + 2U) <= dst_len) {
		sys_put_le16(BT_UUID_RSCS_VAL, &dst[used]);
		used += 2U;
	}
#endif

	return used;
}

static enum app_event_type decode_injected_event(uint8_t code)
{
	switch (code) {
	case 1:
		return APP_EVENT_USER_TAP;
	case 2:
		return APP_EVENT_USER_PET_SOFT;
	case 3:
		return APP_EVENT_USER_PET_LONG;
	case 4:
		return APP_EVENT_USER_HOLD;
	case 5:
		return APP_EVENT_SHAKE_LIGHT;
	case 6:
		return APP_EVENT_SHAKE_PLAY;
	case 7:
		return APP_EVENT_SHAKE_ROUGH;
	case 8:
		return APP_EVENT_WALKING_START;
	case 9:
		return APP_EVENT_WALKING_STOP;
	case 10:
		return APP_EVENT_STEP_BATCH;
	case 11:
		return APP_EVENT_PHONE_NOTIFICATION_SINGLE;
	case 12:
		return APP_EVENT_PHONE_NOTIFICATION_BURST;
	case 13:
		return APP_EVENT_APP_SESSION_START;
	case 14:
		return APP_EVENT_APP_SESSION_END;
	case 15:
		return APP_EVENT_CHARGER_CONNECTED;
	case 16:
		return APP_EVENT_BATTERY_LOW;
	case 17:
		return APP_EVENT_BATTERY_CRITICAL;
	case 18:
		return APP_EVENT_SELF_WAKE_TIMER;
	case 19:
		return APP_EVENT_TIME_SYNC;
	case 20:
		return APP_EVENT_WAKE;
	case 21:
		return APP_EVENT_SLEEP_REQUEST;
	case 22:
		return APP_EVENT_PICKUP_CANDIDATE;
	case 23:
		return APP_EVENT_PICKED_UP;
	case 24:
		return APP_EVENT_IN_HAND_ENTER;
	case 25:
		return APP_EVENT_IN_HAND_EXIT;
	case 26:
		return APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG;
	default:
		return APP_EVENT_COUNT;
	}
}

static ssize_t inject_face_debug_v1(const uint8_t *payload, uint16_t len)
{
	int err;

	if (len < 3U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (payload[1] != KERFUR_FACE_DEBUG_VERSION) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	switch (payload[2]) {
	case KERFUR_FACE_DEBUG_OP_LOOK_TARGET:
		if (len < 8U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: look x=%d y=%d conf=%u",
			(int16_t)sys_get_le16(&payload[3]),
			(int16_t)sys_get_le16(&payload[5]),
			payload[7]);
		err = app_event_publish_look_target((int16_t)sys_get_le16(&payload[3]),
						    (int16_t)sys_get_le16(&payload[5]),
						    payload[7]);
		break;

	case KERFUR_FACE_DEBUG_OP_CARRY_STATE:
		if (len < 7U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: carry in_hand=%u pickup=%u in_hand_conf=%u walk_conf=%u",
			payload[3], payload[4], payload[5], payload[6]);
		err = app_event_publish_carry_state(payload[3] != 0U, payload[4], payload[5], payload[6]);
		break;

	case KERFUR_FACE_DEBUG_OP_BATTERY_PERCENT:
		if (len < 5U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: battery percent=%d known=%u", (int8_t)payload[3], payload[4]);
		err = app_event_publish_battery_percent((int8_t)payload[3], payload[4] != 0U);
		break;

	case KERFUR_FACE_DEBUG_OP_PICKUP_CANDIDATE:
		if (len < 4U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: pickup candidate conf=%u", payload[3]);
		err = app_event_publish_with_timestamp(APP_EVENT_PICKUP_CANDIDATE, payload[3],
						       k_uptime_get());
		break;

	case KERFUR_FACE_DEBUG_OP_PICKED_UP:
		if (len < 4U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: picked up conf=%u", payload[3]);
		err = app_event_publish_with_timestamp(APP_EVENT_PICKED_UP, payload[3],
						       k_uptime_get());
		break;

	case KERFUR_FACE_DEBUG_OP_IN_HAND_ENTER:
		if (len < 4U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: in hand enter conf=%u", payload[3]);
		err = app_event_publish_with_timestamp(APP_EVENT_IN_HAND_ENTER, payload[3],
						       k_uptime_get());
		break;

	case KERFUR_FACE_DEBUG_OP_IN_HAND_EXIT:
		if (len < 4U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: in hand exit conf=%u", payload[3]);
		err = app_event_publish_with_timestamp(APP_EVENT_IN_HAND_EXIT, payload[3],
						       k_uptime_get());
		break;

	case KERFUR_FACE_DEBUG_OP_DYNAMIC_PUPILS:
		if (len < 4U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		LOG_INF("BLE inject face: dynamic pupils %s", payload[3] != 0U ? "disabled" : "enabled");
		err = app_event_publish(APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG,
					payload[3] != 0U ? 1 : 0);
		break;

	default:
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	if (err != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return (ssize_t)len;
}

static void ancs_clear_cached_state(void)
{
	g_ancs.service_start_handle = 0U;
	g_ancs.service_end_handle = 0U;
	g_ancs.notif_source_handle = 0U;
	g_ancs.control_point_handle = 0U;
	g_ancs.data_source_handle = 0U;
	g_ancs.discovery_in_progress = false;
	g_ancs.notif_subscribed = false;
	(void)memset(&g_ancs.service_disc_params, 0, sizeof(g_ancs.service_disc_params));
	(void)memset(&g_ancs.char_disc_params, 0, sizeof(g_ancs.char_disc_params));
	(void)memset(&g_ancs.ccc_disc_params, 0, sizeof(g_ancs.ccc_disc_params));
	/* Do NOT memset notif_sub_params here. This also runs in the disconnect
	 * path, and zeroing a bt_gatt_subscribe_params that Zephyr may still hold
	 * linked in its internal subscription list corrupts that list and hangs the
	 * next connect. Zephyr owns/unlinks it on disconnect (AUTO_RESUBSCRIBE=n);
	 * we re-init it cleanly at subscribe time in
	 * ancs_subscribe_notification_source(). */
}

static void ancs_release_conn(void)
{
	if (g_ancs.conn != NULL) {
		bt_conn_unref(g_ancs.conn);
		g_ancs.conn = NULL;
	}
	ancs_clear_cached_state();
}

static void ans_clear_cached_state(void)
{
	g_ans.service_start_handle = 0U;
	g_ans.service_end_handle = 0U;
	g_ans.new_alert_handle = 0U;
	g_ans.unread_alert_handle = 0U;
	g_ans.discovery_in_progress = false;
	g_ans.new_alert_subscribed = false;
	g_ans.unread_alert_subscribed = false;
	(void)memset(&g_ans.service_disc_params, 0, sizeof(g_ans.service_disc_params));
	(void)memset(&g_ans.char_disc_params, 0, sizeof(g_ans.char_disc_params));
	(void)memset(&g_ans.new_alert_ccc_disc_params, 0, sizeof(g_ans.new_alert_ccc_disc_params));
	(void)memset(&g_ans.unread_alert_ccc_disc_params, 0, sizeof(g_ans.unread_alert_ccc_disc_params));
	/* Do NOT memset the *_sub_params here (see ancs_clear_cached_state): never
	 * zero a bt_gatt_subscribe_params in the disconnect path while Zephyr may
	 * still have it linked. They are re-initialised at subscribe time. */
}

static void ans_release_conn(void)
{
	if (g_ans.conn != NULL) {
		bt_conn_unref(g_ans.conn);
		g_ans.conn = NULL;
	}
	ans_clear_cached_state();
}

static void ancs_subscribe_cb(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err != 0U) {
		g_ancs.notif_subscribed = false;
		LOG_WRN("ANCS Notification Source subscribe failed (att_err=0x%02x)", err);
		return;
	}

	g_ancs.notif_subscribed = true;
	LOG_INF("ANCS Notification Source subscribed");
}

static uint8_t ancs_notif_source_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
				    const void *data, uint16_t length)
{
	const uint8_t *p = data;
	uint8_t event_id;
	uint8_t category_id;
	uint8_t category_count;
	uint32_t notification_uid;

	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (data == NULL) {
		g_ancs.notif_subscribed = false;
		LOG_WRN("ANCS Notification Source unsubscribed");
		return BT_GATT_ITER_STOP;
	}

	if (length < ANCS_NOTIFICATION_SOURCE_LEN) {
		LOG_WRN("ANCS Notification Source payload too short (%u)", length);
		return BT_GATT_ITER_CONTINUE;
	}

	event_id = p[0];
	category_id = p[2];
	category_count = p[3];
	notification_uid = sys_get_le32(&p[4]);

	if (event_id == ANCS_EVENT_ID_NOTIFICATION_ADDED) {
		LOG_INF("ANCS new notification (cat=%u cnt=%u uid=0x%08x)",
			category_id, category_count, notification_uid);
		(void)app_event_publish(APP_EVENT_PHONE_NOTIFICATION, category_id);
	} else {
		LOG_DBG("ANCS event id=%u cat=%u uid=0x%08x",
			event_id, category_id, notification_uid);
	}

	return BT_GATT_ITER_CONTINUE;
}
static int ancs_subscribe_notification_source(struct bt_conn *conn)
{
	int err;

	if (g_ancs.notif_source_handle == 0U) {
		return -ENOENT;
	}

	if (g_ancs.notif_subscribed) {
		return 0;
	}

	(void)memset(&g_ancs.notif_sub_params, 0, sizeof(g_ancs.notif_sub_params));
	(void)memset(&g_ancs.ccc_disc_params, 0, sizeof(g_ancs.ccc_disc_params));

	g_ancs.notif_sub_params.notify = ancs_notif_source_cb;
	g_ancs.notif_sub_params.subscribe = ancs_subscribe_cb;
	g_ancs.notif_sub_params.value = BT_GATT_CCC_NOTIFY;
	g_ancs.notif_sub_params.value_handle = g_ancs.notif_source_handle;
	g_ancs.notif_sub_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
#if defined(CONFIG_BT_GATT_AUTO_DISCOVER_CCC)
	g_ancs.notif_sub_params.end_handle = g_ancs.service_end_handle;
	g_ancs.notif_sub_params.disc_params = &g_ancs.ccc_disc_params;
#endif
#if defined(CONFIG_BT_SMP)
	g_ancs.notif_sub_params.min_security = BT_SECURITY_L2;
#endif

	err = bt_gatt_subscribe(conn, &g_ancs.notif_sub_params);
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("ANCS subscribe request failed (%d)", err);
		return err;
	}

	if (err == -EALREADY) {
		g_ancs.notif_subscribed = true;
		LOG_INF("ANCS Notification Source already subscribed");
	} else {
		LOG_INF("ANCS subscribe request queued");
	}

	return 0;
}

static int ancs_start_discovery(struct bt_conn *conn)
{
	int err;

	if (!IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		return -ENOTSUP;
	}

	if (g_ancs.discovery_in_progress || g_ancs.notif_subscribed) {
		return -EALREADY;
	}

	if ((g_ancs.conn != NULL) && (g_ancs.conn != conn)) {
		ancs_release_conn();
	}
	if (g_ancs.conn == NULL) {
		g_ancs.conn = bt_conn_ref(conn);
	}

	ancs_clear_cached_state();
	g_ancs.discovery_in_progress = true;

	g_ancs.service_disc_params.uuid = &g_ancs_service_uuid.uuid;
	g_ancs.service_disc_params.func = ancs_discover_cb;
	g_ancs.service_disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	g_ancs.service_disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	g_ancs.service_disc_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(conn, &g_ancs.service_disc_params);
	if (err != 0) {
		g_ancs.discovery_in_progress = false;
		LOG_WRN("ANCS service discovery start failed (%d)", err);
		return err;
	}

	LOG_INF("ANCS service discovery started");
	return 0;
}

static void ans_subscribe_state_update(struct bt_gatt_subscribe_params *params, bool enabled)
{
	if (params == &g_ans.new_alert_sub_params) {
		g_ans.new_alert_subscribed = enabled;
	} else if (params == &g_ans.unread_alert_sub_params) {
		g_ans.unread_alert_subscribed = enabled;
	}
}

static void ans_subscribe_cb(struct bt_conn *conn, uint8_t err,
			     struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(conn);

	if (err != 0U) {
		ans_subscribe_state_update(params, false);
		LOG_WRN("ANS subscribe failed (att_err=0x%02x)", err);
		return;
	}

	ans_subscribe_state_update(params, true);
	LOG_INF("ANS characteristic subscribed");
}

static uint8_t ans_new_alert_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
				const void *data, uint16_t length)
{
	const uint8_t *payload = data;

	ARG_UNUSED(conn);

	if (data == NULL) {
		ans_subscribe_state_update(params, false);
		return BT_GATT_ITER_STOP;
	}

	if (length < ANS_NEW_ALERT_MIN_LEN) {
		LOG_WRN("ANS New Alert payload too short (%u)", length);
		return BT_GATT_ITER_CONTINUE;
	}

	LOG_INF("ANS new alert category=%u count=%u", payload[0], payload[1]);
	(void)app_event_publish(APP_EVENT_PHONE_NOTIFICATION, payload[0]);
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t ans_unread_alert_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
				   const void *data, uint16_t length)
{
	const uint8_t *payload = data;

	ARG_UNUSED(conn);

	if (data == NULL) {
		ans_subscribe_state_update(params, false);
		return BT_GATT_ITER_STOP;
	}

	if (length < ANS_UNREAD_ALERT_MIN_LEN) {
		LOG_WRN("ANS Unread Alert payload too short (%u)", length);
		return BT_GATT_ITER_CONTINUE;
	}

	LOG_DBG("ANS unread alert category=%u count=%u", payload[0], payload[1]);
	return BT_GATT_ITER_CONTINUE;
}

static int ans_subscribe_characteristics(struct bt_conn *conn)
{
	int err = 0;
	bool attempted = false;

	if (g_ans.new_alert_handle != 0U && !g_ans.new_alert_subscribed) {
		attempted = true;
		(void)memset(&g_ans.new_alert_sub_params, 0, sizeof(g_ans.new_alert_sub_params));
		(void)memset(&g_ans.new_alert_ccc_disc_params, 0, sizeof(g_ans.new_alert_ccc_disc_params));

		g_ans.new_alert_sub_params.notify = ans_new_alert_cb;
		g_ans.new_alert_sub_params.subscribe = ans_subscribe_cb;
		g_ans.new_alert_sub_params.value = BT_GATT_CCC_NOTIFY;
		g_ans.new_alert_sub_params.value_handle = g_ans.new_alert_handle;
		g_ans.new_alert_sub_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
#if defined(CONFIG_BT_GATT_AUTO_DISCOVER_CCC)
		g_ans.new_alert_sub_params.end_handle = g_ans.service_end_handle;
		g_ans.new_alert_sub_params.disc_params = &g_ans.new_alert_ccc_disc_params;
#endif

		err = bt_gatt_subscribe(conn, &g_ans.new_alert_sub_params);
		if ((err != 0) && (err != -EALREADY)) {
			LOG_WRN("ANS New Alert subscribe failed (%d)", err);
			return err;
		}
		if (err == -EALREADY) {
			g_ans.new_alert_subscribed = true;
		}
	}

	if (g_ans.unread_alert_handle != 0U && !g_ans.unread_alert_subscribed) {
		attempted = true;
		(void)memset(&g_ans.unread_alert_sub_params, 0, sizeof(g_ans.unread_alert_sub_params));
		(void)memset(&g_ans.unread_alert_ccc_disc_params, 0,
			    sizeof(g_ans.unread_alert_ccc_disc_params));

		g_ans.unread_alert_sub_params.notify = ans_unread_alert_cb;
		g_ans.unread_alert_sub_params.subscribe = ans_subscribe_cb;
		g_ans.unread_alert_sub_params.value = BT_GATT_CCC_NOTIFY;
		g_ans.unread_alert_sub_params.value_handle = g_ans.unread_alert_handle;
		g_ans.unread_alert_sub_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
#if defined(CONFIG_BT_GATT_AUTO_DISCOVER_CCC)
		g_ans.unread_alert_sub_params.end_handle = g_ans.service_end_handle;
		g_ans.unread_alert_sub_params.disc_params = &g_ans.unread_alert_ccc_disc_params;
#endif

		err = bt_gatt_subscribe(conn, &g_ans.unread_alert_sub_params);
		if ((err != 0) && (err != -EALREADY)) {
			LOG_WRN("ANS Unread Alert subscribe failed (%d)", err);
			return err;
		}
		if (err == -EALREADY) {
			g_ans.unread_alert_subscribed = true;
		}
	}

	if (!attempted) {
		return -ENOENT;
	}

	LOG_INF("ANS subscribe requests queued");
	return 0;
}
static int ans_start_discovery(struct bt_conn *conn)
{
	int err;

	if (!IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
		return -ENOTSUP;
	}

	if (g_ans.discovery_in_progress || g_ans.new_alert_subscribed || g_ans.unread_alert_subscribed) {
		return -EALREADY;
	}

	if ((g_ans.conn != NULL) && (g_ans.conn != conn)) {
		ans_release_conn();
	}
	if (g_ans.conn == NULL) {
		g_ans.conn = bt_conn_ref(conn);
	}

	ans_clear_cached_state();
	g_ans.discovery_in_progress = true;

	g_ans.service_disc_params.uuid = &g_ans_service_uuid.uuid;
	g_ans.service_disc_params.func = ans_discover_cb;
	g_ans.service_disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	g_ans.service_disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	g_ans.service_disc_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(conn, &g_ans.service_disc_params);
	if (err != 0) {
		g_ans.discovery_in_progress = false;
		LOG_WRN("ANS service discovery start failed (%d)", err);
		return err;
	}

	LOG_INF("ANS service discovery started");
	return 0;
}

static uint8_t ancs_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	int err;

	if (attr == NULL) {
		if (params == &g_ancs.service_disc_params) {
			g_ancs.discovery_in_progress = false;
			LOG_INF("ANCS service not found on peer");
			if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
				(void)ans_start_discovery(conn);
			}
			return BT_GATT_ITER_STOP;
		}

		if (params == &g_ancs.char_disc_params) {
			g_ancs.discovery_in_progress = false;
			if (g_ancs.notif_source_handle == 0U) {
				LOG_WRN("ANCS Notification Source characteristic not found");
				if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
					(void)ans_start_discovery(conn);
				}
				return BT_GATT_ITER_STOP;
			}

			err = ancs_subscribe_notification_source(conn);
			if (err != 0) {
				LOG_WRN("ANCS Notification Source subscribe failed (%d)", err);
			}
			return BT_GATT_ITER_STOP;
		}

		return BT_GATT_ITER_STOP;
	}

	if (params == &g_ancs.service_disc_params) {
		const struct bt_gatt_service_val *service = attr->user_data;

		if (service == NULL) {
			return BT_GATT_ITER_CONTINUE;
		}

		g_ancs.service_start_handle = (uint16_t)(attr->handle + 1U);
		g_ancs.service_end_handle = service->end_handle;
		LOG_INF("ANCS handles: 0x%04x..0x%04x",
			g_ancs.service_start_handle, g_ancs.service_end_handle);

		g_ancs.char_disc_params.uuid = NULL;
		g_ancs.char_disc_params.func = ancs_discover_cb;
		g_ancs.char_disc_params.start_handle = g_ancs.service_start_handle;
		g_ancs.char_disc_params.end_handle = g_ancs.service_end_handle;
		g_ancs.char_disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &g_ancs.char_disc_params);
		if (err != 0) {
			g_ancs.discovery_in_progress = false;
			LOG_WRN("ANCS characteristic discovery start failed (%d)", err);
		} else {
			LOG_INF("ANCS characteristic discovery started");
		}
		return BT_GATT_ITER_STOP;
	}

	if (params == &g_ancs.char_disc_params) {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		if (chrc == NULL) {
			return BT_GATT_ITER_CONTINUE;
		}

		if (bt_uuid_cmp(chrc->uuid, &g_ancs_notif_source_uuid.uuid) == 0) {
			g_ancs.notif_source_handle = chrc->value_handle;
			LOG_INF("ANCS Notification Source handle: 0x%04x", g_ancs.notif_source_handle);
		} else if (bt_uuid_cmp(chrc->uuid, &g_ancs_control_point_uuid.uuid) == 0) {
			g_ancs.control_point_handle = chrc->value_handle;
			LOG_DBG("ANCS Control Point handle: 0x%04x", g_ancs.control_point_handle);
		} else if (bt_uuid_cmp(chrc->uuid, &g_ancs_data_source_uuid.uuid) == 0) {
			g_ancs.data_source_handle = chrc->value_handle;
			LOG_DBG("ANCS Data Source handle: 0x%04x", g_ancs.data_source_handle);
		}
	}

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t ans_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       struct bt_gatt_discover_params *params)
{
	int err;

	if (attr == NULL) {
		if (params == &g_ans.service_disc_params) {
			g_ans.discovery_in_progress = false;
			LOG_INF("ANS service not found on peer");
			return BT_GATT_ITER_STOP;
		}

		if (params == &g_ans.char_disc_params) {
			g_ans.discovery_in_progress = false;
			err = ans_subscribe_characteristics(conn);
			if (err == -ENOENT) {
				LOG_INF("ANS characteristics not exposed by peer");
			} else if (err != 0) {
				LOG_WRN("ANS subscribe failed (%d)", err);
			}
			return BT_GATT_ITER_STOP;
		}

		return BT_GATT_ITER_STOP;
	}

	if (params == &g_ans.service_disc_params) {
		const struct bt_gatt_service_val *service = attr->user_data;

		if (service == NULL) {
			return BT_GATT_ITER_CONTINUE;
		}

		g_ans.service_start_handle = (uint16_t)(attr->handle + 1U);
		g_ans.service_end_handle = service->end_handle;
		LOG_INF("ANS handles: 0x%04x..0x%04x",
			g_ans.service_start_handle, g_ans.service_end_handle);

		g_ans.char_disc_params.uuid = NULL;
		g_ans.char_disc_params.func = ans_discover_cb;
		g_ans.char_disc_params.start_handle = g_ans.service_start_handle;
		g_ans.char_disc_params.end_handle = g_ans.service_end_handle;
		g_ans.char_disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &g_ans.char_disc_params);
		if (err != 0) {
			g_ans.discovery_in_progress = false;
			LOG_WRN("ANS characteristic discovery start failed (%d)", err);
		} else {
			LOG_INF("ANS characteristic discovery started");
		}
		return BT_GATT_ITER_STOP;
	}

	if (params == &g_ans.char_disc_params) {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		if (chrc == NULL) {
			return BT_GATT_ITER_CONTINUE;
		}

		if (bt_uuid_cmp(chrc->uuid, &g_ans_new_alert_uuid.uuid) == 0) {
			g_ans.new_alert_handle = chrc->value_handle;
			LOG_INF("ANS New Alert handle: 0x%04x", g_ans.new_alert_handle);
		} else if (bt_uuid_cmp(chrc->uuid, &g_ans_unread_alert_uuid.uuid) == 0) {
			g_ans.unread_alert_handle = chrc->value_handle;
			LOG_INF("ANS Unread Alert handle: 0x%04x", g_ans.unread_alert_handle);
		}
	}

	return BT_GATT_ITER_CONTINUE;
}

static int phone_notification_discovery_start(struct bt_conn *conn)
{
	int ancs_err = -ENOTSUP;
	int ans_err = -ENOTSUP;

	if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		ancs_err = ancs_start_discovery(conn);
		if ((ancs_err == 0) || (ancs_err == -EALREADY)) {
			return 0;
		}
	}

	if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
		ans_err = ans_start_discovery(conn);
		if ((ans_err == 0) || (ans_err == -EALREADY)) {
			return 0;
		}
	}

	if (ancs_err != -ENOTSUP) {
		return ancs_err;
	}

	return ans_err;
}

static bool notification_discovery_retryable(int err)
{
	return (err == -EBUSY) || (err == -EAGAIN);
}

static void schedule_notification_discovery_retry(void)
{
	if (g_notify_discovery_retries >= KERFUR_NOTIFY_DISCOVERY_MAX_RETRIES) {
		LOG_WRN("Notification discovery retries exhausted");
		return;
	}

	g_notify_discovery_retries++;
	(void)k_work_reschedule(&g_notify_discovery_work, KERFUR_NOTIFY_DISCOVERY_RETRY_DELAY);
	LOG_WRN("Notification discovery retry %u/%u scheduled",
		g_notify_discovery_retries, KERFUR_NOTIFY_DISCOVERY_MAX_RETRIES);
}

static void request_phone_notification_discovery(struct bt_conn *conn)
{
	int disc_err;

	if (conn == NULL) {
		return;
	}

	disc_err = phone_notification_discovery_start(conn);
	if ((disc_err == 0) || (disc_err == -EALREADY)) {
		g_notify_discovery_retries = 0U;
		return;
	}

	if (notification_discovery_retryable(disc_err)) {
		schedule_notification_discovery_retry();
		return;
	}

	g_notify_discovery_retries = 0U;
	LOG_WRN("Notification discovery start failed (%d)", disc_err);
}

static ssize_t event_inject_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *payload = buf;
	enum app_event_type type;
	int32_t param = 0;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (payload[0] == KERFUR_FACE_DEBUG_MAGIC) {
		return inject_face_debug_v1(payload, len);
	}

	type = decode_injected_event(payload[0]);
	if (type == APP_EVENT_COUNT) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	if (len >= 2U) {
		param = payload[1];
	}

	if ((type == APP_EVENT_STEP_BATCH) && (param == 0)) {
		param = 8;
	}

	if ((type == APP_EVENT_TIME_SYNC) && (len >= 5U)) {
		param = (int32_t)sys_get_le32(&payload[1]);
	}

	LOG_INF("BLE inject: code=%u -> %s (param=%d)", payload[0], app_event_type_str(type), param);
	(void)app_event_publish(type, param);
	return len;
}

BT_GATT_SERVICE_DEFINE(kerfur_debug_svc,
	BT_GATT_PRIMARY_SERVICE(&g_kerfur_debug_service_uuid),
	BT_GATT_CHARACTERISTIC(&g_kerfur_event_inject_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, event_inject_write_cb, NULL),
);

#if defined(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) && CONFIG_KERFUR_ENABLE_KEYRING_PROFILE
static bool keyring_alert_level_valid(uint8_t level)
{
	return level <= KERFUR_ALERT_LEVEL_HIGH_ALERT;
}

static ssize_t lls_read_alert_level_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(g_lls_alert_level));
}

static ssize_t lls_write_alert_level_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t level;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	level = ((const uint8_t *)buf)[0];
	if (!keyring_alert_level_valid(level)) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	g_lls_alert_level = level;
	LOG_INF("LLS alert level set to %u", level);
	return len;
}

BT_GATT_SERVICE_DEFINE(kerfur_lls_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_LLS),
	BT_GATT_CHARACTERISTIC(BT_UUID_ALERT_LEVEL,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
				       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       lls_read_alert_level_cb, lls_write_alert_level_cb, &g_lls_alert_level),
);
#endif

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
static void companion_notify_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	g_companion_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Companion notifications %s", g_companion_notify_enabled ? "enabled" : "disabled");
}

static ssize_t companion_tx_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, g_companion_last_tx,
				 g_companion_last_tx_len);
}

static void companion_log_payload_preview(const uint8_t *payload, uint16_t len)
{
	char preview[17];
	size_t i;
	size_t copy_len;

	copy_len = MIN((size_t)len, sizeof(preview) - 1U);
	(void)memcpy(preview, payload, copy_len);
	preview[copy_len] = '\0';

	for (i = 0; i < copy_len; i++) {
		if ((preview[i] < 0x20) || (preview[i] > 0x7e)) {
			preview[i] = '.';
		}
	}

	LOG_INF("Companion text preview: %s", preview);
}

static ssize_t companion_rx_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *payload = buf;
	uint8_t response[3];
	int err;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (payload[0]) {
	case KERFUR_GB_OP_PING:
		response[0] = KERFUR_GB_EVT_PONG;
		response[1] = KERFUR_GB_PROTO_VERSION;
		err = companion_send_packet(response, 2U);
		if ((err != 0) && (err != -EACCES) && (err != -ENOTCONN)) {
			LOG_WRN("Companion PING response failed (%d)", err);
		}
		return len;

	case KERFUR_GB_OP_ANDROID_NOTIFICATION:
		if (len < 2U) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		LOG_INF("Companion Android notification category=%u", payload[1]);
		if (len > 2U) {
			companion_log_payload_preview(&payload[2], len - 2U);
		}

		(void)app_event_publish(APP_EVENT_PHONE_NOTIFICATION, payload[1]);
		response[0] = KERFUR_GB_EVT_NOTIFICATION_ACK;
		response[1] = payload[1];
		err = companion_send_packet(response, 2U);
		if ((err != 0) && (err != -EACCES) && (err != -ENOTCONN)) {
			LOG_WRN("Companion notification ACK failed (%d)", err);
		}
		return len;

	default:
		response[0] = KERFUR_GB_EVT_ERROR;
		response[1] = KERFUR_GB_ERR_UNSUPPORTED_OPCODE;
		response[2] = payload[0];
		err = companion_send_packet(response, 3U);
		if ((err != 0) && (err != -EACCES) && (err != -ENOTCONN)) {
			LOG_WRN("Companion error response failed (%d)", err);
		}
		return len;
	}
}

BT_GATT_SERVICE_DEFINE(kerfur_companion_svc,
	BT_GATT_PRIMARY_SERVICE(&g_kerfur_companion_service_uuid),
	BT_GATT_CHARACTERISTIC(&g_kerfur_companion_rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, companion_rx_write_cb, NULL),
	BT_GATT_CHARACTERISTIC(&g_kerfur_companion_tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, companion_tx_read_cb, NULL, NULL),
	BT_GATT_CCC(companion_notify_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static int companion_send_packet(const uint8_t *payload, uint8_t len)
{
	int err;

	if ((payload == NULL) || (len == 0U)) {
		return -EINVAL;
	}

	if (len > sizeof(g_companion_last_tx)) {
		return -EMSGSIZE;
	}

	if (g_active_conn == NULL) {
		return -ENOTCONN;
	}

	if (!g_companion_notify_enabled) {
		return -EACCES;
	}

	(void)memcpy(g_companion_last_tx, payload, len);
	g_companion_last_tx_len = len;

	err = bt_gatt_notify(g_active_conn, &kerfur_companion_svc.attrs[KERFUR_COMPANION_TX_ATTR_INDEX],
			     payload, len);
	if (err != 0) {
		LOG_WRN("Companion notify failed (%d)", err);
	}

	return err;
}
#endif

#if defined(CONFIG_KERFUR_ENABLE_RSC_SERVICE) && CONFIG_KERFUR_ENABLE_RSC_SERVICE
static void rsc_meas_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	g_rsc_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_rsc_feature_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				   uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data, sizeof(g_rsc_feature));
}

static ssize_t read_rsc_sensor_location_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					   void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(g_rsc_sensor_location));
}

BT_GATT_SERVICE_DEFINE(kerfur_rsc_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_RSCS),
	BT_GATT_CHARACTERISTIC(BT_UUID_RSC_MEASUREMENT,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, g_rsc_measurement),
	BT_GATT_CCC(rsc_meas_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_RSC_FEATURE,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_rsc_feature_cb, NULL, &g_rsc_feature),
	BT_GATT_CHARACTERISTIC(BT_UUID_SENSOR_LOCATION,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_rsc_sensor_location_cb, NULL, &g_rsc_sensor_location),
);
#endif
static void request_low_power_conn_params(void)
{
	int err;

	if (g_active_conn == NULL) {
		return;
	}

	err = bt_conn_le_param_update(g_active_conn, g_low_power_conn_param);
	if (err == -EALREADY) {
		return;
	}

	if (err == -EBUSY) {
		(void)k_work_schedule(&g_conn_param_work, KERFUR_CONN_PARAM_RETRY_DELAY);
		return;
	}

	if (err != 0) {
		LOG_WRN("conn_param_update failed (%d)", err);
	} else {
		LOG_INF("Requested low-power connection parameters");
	}
}

static void conn_param_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	request_low_power_conn_params();
}

static void notify_discovery_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_active_conn == NULL) {
		g_notify_discovery_retries = 0U;
		return;
	}

	request_phone_notification_discovery(g_active_conn);
}

static bool adv_restart_retryable(int err)
{
	return (err == -ENOMEM) || (err == -EBUSY) || (err == -EAGAIN);
}

static void adv_restart_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (g_active_conn != NULL) {
		g_adv_restart_retries = 0U;
		return;
	}

	err = ble_start_advertising();
	if (err == 0) {
		g_adv_restart_retries = 0U;
		return;
	}

	if (adv_restart_retryable(err) &&
	    (g_adv_restart_retries < KERFUR_ADV_RESTART_MAX_RETRIES)) {
		g_adv_restart_retries++;
		(void)k_work_reschedule(&g_adv_restart_work, KERFUR_ADV_RESTART_RETRY_DELAY);
		LOG_WRN("Advertising restart retry %u/%u scheduled (err=%d)",
			g_adv_restart_retries, KERFUR_ADV_RESTART_MAX_RETRIES, err);
		return;
	}

	LOG_WRN("Advertising restart failed (%d)", err);
	g_adv_restart_retries = 0U;
}

static void adv_phase_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (g_active_conn != NULL) {
		return;
	}

	if (g_adv_phase == BLE_ADV_PHASE_FAST) {
		err = ble_set_advertising_phase(BLE_ADV_PHASE_SLOW, true);
		if (err != 0) {
			LOG_WRN("Failed switching fast->slow advertising (%d)", err);
			(void)k_work_reschedule(&g_adv_restart_work,
						KERFUR_ADV_RESTART_RETRY_DELAY);
		}
	}
}

static int ble_adv_start_with_payload(const struct bt_le_adv_param *param,
				      const struct bt_data *ad, size_t ad_len,
				      const struct bt_data *sd, size_t sd_len)
{
	int err;

	err = bt_le_adv_start(param, ad, ad_len, sd, sd_len);
	if (err == -EALREADY) {
		return 0;
	}

	if ((err != 0) && (sd_len > 0U)) {
		LOG_WRN("Advertising with scan response failed (%d), retrying minimal", err);
		err = bt_le_adv_start(param, ad, ad_len, NULL, 0);
		if (err == -EALREADY) {
			return 0;
		}
	}

	return err;
}

static int ble_set_advertising_phase(enum ble_adv_phase phase, bool restart)
{
	int err;
	const struct bt_le_adv_param *adv_param;

	if (restart) {
		err = bt_le_adv_stop();
		if ((err != 0) && (err != -EALREADY)) {
			LOG_WRN("bt_le_adv_stop failed (%d)", err);
		}
	}

	switch (phase) {
	case BLE_ADV_PHASE_FAST:
		adv_param = &g_adv_param_fast;
		err = ble_adv_start_with_payload(adv_param, g_adv_data, g_adv_data_len,
						g_scan_rsp_data, g_scan_rsp_data_len);
		if (err != 0) {
			return err;
		}

		g_adv_phase = BLE_ADV_PHASE_FAST;
		LOG_INF("Advertising phase: fast undirected");
		break;

	case BLE_ADV_PHASE_SLOW:
		adv_param = &g_adv_param_slow;
		err = ble_adv_start_with_payload(adv_param, g_adv_data, g_adv_data_len,
						g_scan_rsp_data, g_scan_rsp_data_len);
		if (err != 0) {
			return err;
		}

		g_adv_phase = BLE_ADV_PHASE_SLOW;
		LOG_INF("Advertising phase: slow undirected");
		break;

	default:
		return -EINVAL;
	}

	reset_adv_timers();
	return 0;
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	int sec_err;

	if (err != 0U) {
		LOG_WRN("BLE connect failed (err=0x%02x)", err);
		(void)k_work_reschedule(&g_adv_restart_work,
					KERFUR_ADV_RESTART_INITIAL_DELAY);
		return;
	}

	if (g_active_conn != NULL) {
		bt_conn_unref(g_active_conn);
	}
	g_active_conn = bt_conn_ref(conn);

	(void)k_work_cancel_delayable(&g_adv_phase_work);
	(void)k_work_cancel_delayable(&g_notify_discovery_work);
	(void)k_work_cancel_delayable(&g_adv_restart_work);
	g_notify_discovery_retries = 0U;
	g_adv_restart_retries = 0U;
	g_adv_phase = BLE_ADV_PHASE_IDLE;

	ancs_release_conn();
	ans_release_conn();

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
	g_companion_notify_enabled = false;
#endif

	(void)app_event_publish(APP_EVENT_BLE_CONNECTED, 0);
	LOG_INF("BLE connected");

	request_low_power_conn_params();

	if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		sec_err = bt_conn_set_security(conn, BT_SECURITY_L2);
		if ((sec_err != 0) && (sec_err != -EALREADY) && (sec_err != -EBUSY)) {
			LOG_WRN("BLE security request failed (%d)", sec_err);
		} else {
			LOG_INF("Requested encrypted link for ANCS");
		}

		if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
			request_phone_notification_discovery(conn);
		}
	} else if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
		request_phone_notification_discovery(conn);
	}
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	(void)app_event_publish(APP_EVENT_BLE_DISCONNECTED, reason);
	LOG_INF("BLE disconnected (reason 0x%02x)", reason);
#if defined(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) && CONFIG_KERFUR_ENABLE_KEYRING_PROFILE
	if ((g_lls_alert_level != KERFUR_ALERT_LEVEL_NO_ALERT) &&
	    (reason != BT_HCI_ERR_REMOTE_USER_TERM_CONN)) {
		LOG_WRN("Link Loss alert active (level=%u, reason=0x%02x)",
			g_lls_alert_level, reason);
	}
#endif

	if (g_ancs.conn == conn) {
		ancs_release_conn();
	}
	if (g_ans.conn == conn) {
		ans_release_conn();
	}

	if (g_active_conn == conn) {
		bt_conn_unref(g_active_conn);
		g_active_conn = NULL;
	}

#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
	g_companion_notify_enabled = false;
#endif

	(void)k_work_cancel_delayable(&g_conn_param_work);
	(void)k_work_cancel_delayable(&g_notify_discovery_work);
	(void)k_work_cancel_delayable(&g_adv_restart_work);
	g_notify_discovery_retries = 0U;
	g_adv_restart_retries = 0U;

	{
		/* Normally re-advertise almost immediately so the phone can
		 * reconnect. After a stale-bond security failure, honour the
		 * escalating backoff so we don't loop. One-shot: consume it. */
		const k_timeout_t adv_delay = (g_repair_adv_delay_ms > 0)
			? K_MSEC(g_repair_adv_delay_ms)
			: KERFUR_ADV_RESTART_INITIAL_DELAY;

		g_repair_adv_delay_ms = 0;
		(void)k_work_reschedule(&g_adv_restart_work, adv_delay);
	}
}

#if defined(CONFIG_BT_SMP)
static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS) {
		LOG_WRN("BLE security changed with error (%d: %s)", err, bt_security_err_to_str(err));

		/* PIN_OR_KEY_MISSING is the unambiguous "our stored key no longer
		 * matches the peer" signal — common after a UF2 reflash, or when the
		 * phone has forgotten us. It makes every reconnect fail to encrypt, so
		 * ANCS never subscribes (the "reset a few times" symptom). Drop the
		 * dead bond and the link so the next connection re-pairs cleanly on
		 * its own. (We don't wipe on the broader AUTH_FAIL, which can be a
		 * transient glitch on an otherwise-valid bond.) */
		if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			const bt_addr_le_t *peer = bt_conn_get_dst(conn);
			const int64_t now = k_uptime_get();

			if (peer != NULL) {
				int unpair_err = bt_unpair(BT_ID_DEFAULT, peer);

				if (unpair_err == 0) {
					LOG_WRN("Cleared stale bond after security failure; re-pair from the phone");
				} else if (unpair_err != -ENOENT) {
					LOG_WRN("bt_unpair failed (%d)", unpair_err);
				}
			}

			/* The phone keeps a bond we lost and will instantly reconnect
			 * and fail again. Escalate the re-advertise delay so we stop
			 * looping ~1/s; the disconnect below consumes this delay. */
			if ((now - g_repair_last_fail_ms) > KERFUR_REPAIR_STREAK_RESET_MS) {
				g_repair_fail_streak = 0U;
			}
			g_repair_last_fail_ms = now;
			if (g_repair_fail_streak < 255U) {
				g_repair_fail_streak++;
			}
			g_repair_adv_delay_ms =
				(int64_t)g_repair_fail_streak * KERFUR_REPAIR_BACKOFF_STEP_MS;
			if (g_repair_adv_delay_ms > KERFUR_REPAIR_BACKOFF_MAX_MS) {
				g_repair_adv_delay_ms = KERFUR_REPAIR_BACKOFF_MAX_MS;
			}
			LOG_WRN("Re-pair backoff: advertising paused %lld ms (streak %u)",
				(long long)g_repair_adv_delay_ms, g_repair_fail_streak);

			(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
			return;
		}

		if (err == BT_SECURITY_ERR_AUTH_REQUIREMENT) {
			LOG_WRN("ANCS requires successful pairing; retry after re-pairing from iOS");
			return;
		}
		if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
			(void)ans_start_discovery(conn);
		}
		return;
	}

	LOG_INF("BLE security level now %u", level);

	/* A successful secure link means the bond is good again — clear the
	 * re-pair backoff so normal reconnects stay snappy. */
	g_repair_fail_streak = 0U;
	g_repair_adv_delay_ms = 0;

	if ((level >= BT_SECURITY_L2) || !IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		request_phone_notification_discovery(conn);
	} else if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK)) {
		(void)ans_start_discovery(conn);
	}
}
#endif

BT_CONN_CB_DEFINE(g_bt_conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
#if defined(CONFIG_BT_SMP)
	.security_changed = security_changed_cb,
#endif
};

#if defined(CONFIG_BT_SMP)
static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
	LOG_INF("BLE pairing complete (bonded=%s)", bonded ? "yes" : "no");

	if (!bonded || (conn == NULL)) {
		return;
	}

	if ((bt_conn_get_security(conn) >= BT_SECURITY_L2) || !IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		request_phone_notification_discovery(conn);
	}

	if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		LOG_INF("iOS ANCS hint: enable 'Share System Notifications' for this accessory");
	}
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("BLE pairing failed (%d: %s)", reason, bt_security_err_to_str(reason));

	if (reason == BT_SECURITY_ERR_AUTH_REQUIREMENT) {
		LOG_WRN("Peer rejected pairing auth requirements (possible MITM/IO capability mismatch)");
	}

	if ((reason == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) ||
	    (reason == BT_SECURITY_ERR_AUTH_FAIL) ||
	    (reason == BT_SECURITY_ERR_AUTH_REQUIREMENT)) {
		const bt_addr_le_t *peer = bt_conn_get_dst(conn);

		if (peer != NULL) {
			int unpair_err = bt_unpair(BT_ID_DEFAULT, peer);

			if (unpair_err == 0) {
				LOG_WRN("Removed stale bond for peer (pairing err=%d)", reason);
			} else if (unpair_err != -ENOENT) {
				LOG_WRN("bt_unpair failed (%d)", unpair_err);
			}
		}
	}
}

static void auth_cancel_cb(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("BLE pairing canceled for %s", addr);
}

static void auth_pairing_confirm_cb(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BLE pairing confirm request from %s (auto-accept)", addr);

	err = bt_conn_auth_pairing_confirm(conn);
	if (err != 0) {
		LOG_WRN("bt_conn_auth_pairing_confirm failed (%d)", err);
	}
}

/* No passkey_display / passkey_confirm on purpose: registering neither gives
 * the device IO capability NoInputNoOutput, so a no-display companion pairs via
 * "Just Works" and iOS shows the simple "Bluetooth Pairing Request -> Pair"
 * dialog. We only auto-accept the pairing (no MITM; CONFIG_BT_SMP_ENFORCE_MITM
 * is n). This is the direct fix for the pairing prompt not appearing. */
static struct bt_conn_auth_cb g_bt_auth_cb = {
	.pairing_confirm = auth_pairing_confirm_cb,
	.cancel = auth_cancel_cb,
};

static struct bt_conn_auth_info_cb g_bt_auth_info_cb = {
	.pairing_complete = pairing_complete_cb,
	.pairing_failed = pairing_failed_cb,
};
#endif

static void build_advertising_payloads(void)
{
	const char *dev_name = bt_get_name();
	size_t dev_name_len;
	uint8_t name_type;

	dev_name_len = strlen(dev_name);
	name_type = BT_DATA_NAME_COMPLETE;
	if (dev_name_len > KERFUR_ADV_MAX_NAME_LEN) {
		dev_name_len = KERFUR_ADV_MAX_NAME_LEN;
		name_type = BT_DATA_NAME_SHORTENED;
	}

	(void)memcpy(g_adv_name, dev_name, dev_name_len);
	g_adv_name[dev_name_len] = '\0';

	g_adv_uuid16_visible_len = build_visible_uuid16_payload(g_adv_uuid16_visible,
						sizeof(g_adv_uuid16_visible));

	g_adv_data_len = 0U;
	g_adv_data[g_adv_data_len++] = (struct bt_data){
		.type = BT_DATA_FLAGS,
		.data_len = sizeof(g_adv_flags),
		.data = g_adv_flags,
	};
	g_adv_data[g_adv_data_len++] = (struct bt_data){
		.type = name_type,
		.data_len = dev_name_len,
		.data = g_adv_name,
	};
	if (IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS)) {
		g_adv_data[g_adv_data_len++] = (struct bt_data){
			.type = BT_DATA_SOLICIT128,
			.data_len = sizeof(g_ancs_solicit_uuid),
			.data = g_ancs_solicit_uuid,
		};
	}

	g_scan_rsp_data_len = 0U;
#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
	g_scan_rsp_data[g_scan_rsp_data_len++] = (struct bt_data){
		.type = BT_DATA_UUID128_SOME,
		.data_len = sizeof(g_kerfur_companion_uuid_ad),
		.data = g_kerfur_companion_uuid_ad,
	};
#endif
	if (g_adv_uuid16_visible_len > 0U) {
		g_scan_rsp_data[g_scan_rsp_data_len++] = (struct bt_data){
			.type = BT_DATA_UUID16_SOME,
			.data_len = g_adv_uuid16_visible_len,
			.data = g_adv_uuid16_visible,
		};
	}
	g_scan_rsp_data[g_scan_rsp_data_len++] = (struct bt_data){
		.type = BT_DATA_GAP_APPEARANCE,
		.data_len = sizeof(g_adv_appearance_keyring),
		.data = g_adv_appearance_keyring,
	};

#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
	g_kerfur_beacon_len = kerfur_nearby_build_beacon(g_kerfur_beacon_buf,
							 sizeof(g_kerfur_beacon_buf));
	if ((g_kerfur_beacon_len > 0U) &&
	    (g_scan_rsp_data_len < ARRAY_SIZE(g_scan_rsp_data))) {
		g_scan_rsp_data[g_scan_rsp_data_len++] = (struct bt_data){
			.type = BT_DATA_MANUFACTURER_DATA,
			.data_len = g_kerfur_beacon_len,
			.data = g_kerfur_beacon_buf,
		};
	}
#endif
}

static int ble_start_advertising(void)
{
	int err;
	int stop_err;

	build_advertising_payloads();

	stop_err = bt_le_adv_stop();
	if ((stop_err != 0) && (stop_err != -EALREADY)) {
		LOG_WRN("bt_le_adv_stop before restart failed (%d)", stop_err);
	}

	/* Always connectable-undirected. Directed reconnect was dropped: it could
	 * only be answered by one specific bonded peer, which silently blocked a
	 * fresh phone from connecting (and stalled entirely on a stale bond). */
	err = ble_set_advertising_phase(BLE_ADV_PHASE_FAST, false);
	if (err != 0) {
		return err;
	}

	LOG_INF("Advertising ready: name='%s'%s, ANCS=%s, ANS fallback=%s, keyring=%s, companion=%s",
		g_adv_name,
		(strlen(bt_get_name()) > strlen((const char *)g_adv_name)) ? " (short)" : "",
		IS_ENABLED(CONFIG_KERFUR_ENABLE_ANCS) ? "on" : "off",
		IS_ENABLED(CONFIG_KERFUR_ENABLE_ANS_FALLBACK) ? "on" : "off",
		IS_ENABLED(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) ? "on" : "off",
		IS_ENABLED(CONFIG_KERFUR_ENABLE_COMPANION) ? "on" : "off");

	return 0;
}

int ble_manager_restart_advertising(void)
{
	int err;

	if (!IS_ENABLED(CONFIG_KERFUR_ENABLE_BLE)) {
		return -ENOTSUP;
	}

	if (g_active_conn != NULL) {
		return 0;
	}

	err = ble_start_advertising();
	if (err == 0) {
		g_adv_restart_retries = 0U;
		return 0;
	}

	if (adv_restart_retryable(err) &&
	    (g_adv_restart_retries < KERFUR_ADV_RESTART_MAX_RETRIES)) {
		g_adv_restart_retries++;
		(void)k_work_reschedule(&g_adv_restart_work, KERFUR_ADV_RESTART_RETRY_DELAY);
		LOG_WRN("Advertising restart deferred %u/%u (err=%d)",
			g_adv_restart_retries, KERFUR_ADV_RESTART_MAX_RETRIES, err);
		return 0;
	}

	return err;
}

int ble_manager_disconnect_current(void)
{
	if (g_active_conn == NULL) {
		return -ENOTCONN;
	}

	return bt_conn_disconnect(g_active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

int ble_manager_unpair_all(void)
{
	return bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

bool ble_manager_is_connected(void)
{
	return g_active_conn != NULL;
}

bool ble_manager_is_companion_subscribed(void)
{
#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
	return g_companion_notify_enabled;
#else
	return false;
#endif
}

int ble_manager_companion_send_test_notification(uint8_t category_id)
{
#if defined(CONFIG_KERFUR_ENABLE_COMPANION) && CONFIG_KERFUR_ENABLE_COMPANION
	const uint8_t packet[2] = {
		KERFUR_GB_EVT_TEST_NOTIFICATION,
		category_id,
	};

	return companion_send_packet(packet, sizeof(packet));
#else
	ARG_UNUSED(category_id);
	return -ENOTSUP;
#endif
}

int ble_manager_rsc_notify(uint16_t speed_256ms, uint8_t cadence_spm, bool running)
{
#if defined(CONFIG_KERFUR_ENABLE_RSC_SERVICE) && CONFIG_KERFUR_ENABLE_RSC_SERVICE
	int err;

	g_rsc_measurement[0] = running ? KERFUR_RSC_FLAG_RUNNING : 0U;
	sys_put_le16(speed_256ms, &g_rsc_measurement[1]);
	g_rsc_measurement[3] = cadence_spm;

	if ((g_active_conn == NULL) || !g_rsc_notify_enabled) {
		return -EAGAIN;
	}

	err = bt_gatt_notify(g_active_conn,
			     &kerfur_rsc_svc.attrs[KERFUR_RSC_MEAS_ATTR_INDEX],
			     g_rsc_measurement, sizeof(g_rsc_measurement));
	if (err != 0) {
		LOG_WRN("RSC notify failed (%d)", err);
	}

	return err;
#else
	ARG_UNUSED(speed_256ms);
	ARG_UNUSED(cadence_spm);
	ARG_UNUSED(running);
	return -ENOTSUP;
#endif
}
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
static void kerfur_rotate_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	kerfur_nearby_rotate_ephemeral_id(k_uptime_get());
	(void)k_work_reschedule(&g_adv_restart_work, KERFUR_ADV_RESTART_INITIAL_DELAY);
	(void)k_work_reschedule(&g_kerfur_rotate_work,
				K_SECONDS(CONFIG_KERFUR_NEARBY_ID_ROTATE_S));
}

/* Rate-limited beacon refresh: reuse the proven advertising-restart path so a
 * freshly emitted social event (greet/play) goes out within ~1 s. Requests
 * coalesce (>= KERFUR_BEACON_REFRESH_MIN_MS apart) so a handshake flurry can't
 * churn the radio; the restart handler no-ops while a central is connected. */
static int64_t g_last_beacon_refresh_ms;

void ble_manager_request_beacon_refresh(void)
{
	int64_t now = k_uptime_get();
	int64_t earliest = g_last_beacon_refresh_ms + KERFUR_BEACON_REFRESH_MIN_MS;
	int64_t delay_ms;

	if (now >= earliest) {
		delay_ms = 0;
		g_last_beacon_refresh_ms = now;
	} else {
		delay_ms = earliest - now;
		g_last_beacon_refresh_ms = earliest;
	}

	(void)k_work_reschedule(&g_adv_restart_work, K_MSEC(delay_ms));
}

/* -- Kerfur scan path ------------------------------------------------------ */

#define KERFUR_SCAN_Q_LEN 16

struct kerfur_scan_params {
	uint16_t window_ms;
	uint16_t interval_ms;
	uint16_t period_ms;
	bool     enabled;
};

K_MSGQ_DEFINE(g_kerfur_scan_q, sizeof(struct kerfur_scan_candidate),
	      KERFUR_SCAN_Q_LEN, 4);

static struct k_work_delayable g_kerfur_scan_start_work;
static struct k_work_delayable g_kerfur_scan_stop_work;
static struct k_work g_kerfur_scan_drain_work;
static bool g_kerfur_scan_active;
static struct kerfur_scan_params g_kerfur_scan_params;

static struct kerfur_scan_params kerfur_select_scan_params(
	const struct kerfur_pet_snapshot *snap)
{
	struct kerfur_scan_params p = { .window_ms = 60, .interval_ms = 80,
					.period_ms = 3000, .enabled = true };

	if (snap->battery_critical) {
		p.enabled = false;
		return p;
	}

	switch (snap->mode) {
	case PET_MODE_ASLEEP:
	case PET_MODE_DROWSY:
		p.window_ms = 30; p.interval_ms = 60; p.period_ms = 8000;
		break;
	case PET_MODE_IDLE:
		p.window_ms = 60; p.interval_ms = 80; p.period_ms = 3000;
		break;
	case PET_MODE_WALK_AWAKE:
		p.window_ms = 120; p.interval_ms = 160; p.period_ms = 1000;
		break;
	case PET_MODE_INTERACTING:
		p.window_ms = 100; p.interval_ms = 120; p.period_ms = 800;
		break;
	case PET_MODE_CHARGING:
		p.window_ms = 100; p.interval_ms = 120; p.period_ms = 2000;
		break;
	case PET_MODE_LOW_POWER:
		p.window_ms = 30; p.interval_ms = 80; p.period_ms = 10000;
		break;
	default:
		p.window_ms = 60; p.interval_ms = 80; p.period_ms = 3000;
		break;
	}

	if (snap->battery_low && (p.period_ms < 5000)) {
		p.period_ms = 5000;
	}

	return p;
}

static void kerfur_scan_adv_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
			       struct net_buf_simple *buf)
{
	struct kerfur_scan_candidate candidate = {0};
	bool parsed = false;

	ARG_UNUSED(addr);

	if ((adv_type != BT_GAP_ADV_TYPE_SCAN_RSP) &&
	    (adv_type != BT_GAP_ADV_TYPE_ADV_IND) &&
	    (adv_type != BT_GAP_ADV_TYPE_ADV_SCAN_IND)) {
		return;
	}

	while (buf->len > 1U) {
		uint8_t field_len = net_buf_simple_pull_u8(buf);
		uint8_t field_type;

		if ((field_len == 0U) || (field_len > buf->len)) {
			return;
		}

		field_type = net_buf_simple_pull_u8(buf);
		field_len--;

		if (field_type == BT_DATA_MANUFACTURER_DATA) {
			if (kerfur_nearby_parse_beacon(buf->data, field_len, &candidate)) {
				candidate.rssi = rssi;
				candidate.timestamp_ms = k_uptime_get();
				parsed = true;
			}
			net_buf_simple_pull(buf, field_len);
			break;
		}

		net_buf_simple_pull(buf, field_len);
	}

	if (parsed) {
		if (k_msgq_put(&g_kerfur_scan_q, &candidate, K_NO_WAIT) == 0) {
			(void)k_work_submit(&g_kerfur_scan_drain_work);
		}
	}
}

static void kerfur_scan_drain_work_handler(struct k_work *work)
{
	struct kerfur_scan_candidate candidate;

	ARG_UNUSED(work);

	while (k_msgq_get(&g_kerfur_scan_q, &candidate, K_NO_WAIT) == 0) {
		kerfur_nearby_ingest_candidate(&candidate);
	}
}

static void kerfur_scan_stop_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (!g_kerfur_scan_active) {
		return;
	}

	err = bt_le_scan_stop();
	if ((err != 0) && (err != -EALREADY)) {
		LOG_DBG("bt_le_scan_stop err=%d", err);
	}
	g_kerfur_scan_active = false;
}

static void kerfur_scan_start_work_handler(struct k_work *work)
{
	struct kerfur_pet_snapshot snap;
	struct kerfur_scan_params params;
	struct bt_le_scan_param scan_param = {0};
	int err;

	ARG_UNUSED(work);

	kerfur_nearby_get_snapshot(&snap);
	params = kerfur_select_scan_params(&snap);
	g_kerfur_scan_params = params;

	if (!params.enabled) {
		if (g_kerfur_scan_active) {
			(void)bt_le_scan_stop();
			g_kerfur_scan_active = false;
		}
		/* Retry every 5s while scanning is suppressed. */
		(void)k_work_reschedule(&g_kerfur_scan_start_work, K_SECONDS(5));
		return;
	}

	if (g_kerfur_scan_active) {
		(void)bt_le_scan_stop();
		g_kerfur_scan_active = false;
	}

	scan_param.type = BT_LE_SCAN_TYPE_ACTIVE;
	scan_param.options = BT_LE_SCAN_OPT_NONE;
	scan_param.interval = (uint16_t)((params.interval_ms * 8) / 5); /* ms → 0.625 ms */
	scan_param.window = (uint16_t)((params.window_ms * 8) / 5);

	err = bt_le_scan_start(&scan_param, kerfur_scan_adv_cb);
	if (err == 0) {
		g_kerfur_scan_active = true;
		(void)k_work_reschedule(&g_kerfur_scan_stop_work, K_MSEC(params.window_ms));
	} else if (err != -EALREADY) {
		LOG_WRN("bt_le_scan_start err=%d (window=%ums interval=%ums)", err,
			params.window_ms, params.interval_ms);
	}

	(void)k_work_reschedule(&g_kerfur_scan_start_work, K_MSEC(params.period_ms));
}
#endif /* CONFIG_KERFUR_ENABLE_NEARBY */

int ble_manager_init(void)
{
	int err;

	if (!IS_ENABLED(CONFIG_KERFUR_ENABLE_BLE)) {
		LOG_INF("BLE disabled by Kconfig");
		return 0;
	}

	k_work_init_delayable(&g_adv_phase_work, adv_phase_work_handler);
	k_work_init_delayable(&g_conn_param_work, conn_param_work_handler);
	k_work_init_delayable(&g_notify_discovery_work, notify_discovery_work_handler);
	k_work_init_delayable(&g_adv_restart_work, adv_restart_work_handler);
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
	k_work_init_delayable(&g_kerfur_rotate_work, kerfur_rotate_work_handler);
	k_work_init_delayable(&g_kerfur_scan_start_work, kerfur_scan_start_work_handler);
	k_work_init_delayable(&g_kerfur_scan_stop_work, kerfur_scan_stop_work_handler);
	k_work_init(&g_kerfur_scan_drain_work, kerfur_scan_drain_work_handler);
	g_kerfur_scan_active = false;
#endif
	g_notify_discovery_retries = 0U;
	g_adv_restart_retries = 0U;
#if defined(CONFIG_KERFUR_ENABLE_KEYRING_PROFILE) && CONFIG_KERFUR_ENABLE_KEYRING_PROFILE
	g_lls_alert_level = KERFUR_ALERT_LEVEL_NO_ALERT;
#endif

	ancs_clear_cached_state();
	ans_clear_cached_state();

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

#if defined(CONFIG_BT_SMP)
	err = bt_conn_auth_cb_register(&g_bt_auth_cb);
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("Auth callback register failed (%d)", err);
	}

	err = bt_conn_auth_info_cb_register(&g_bt_auth_info_cb);
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("Auth info callback register failed (%d)", err);
	}
#endif

#if defined(CONFIG_SETTINGS)
	err = settings_load();
	if (err != 0) {
		LOG_WRN("settings_load failed (%d)", err);
	}
#endif

	err = ble_start_advertising();
	if (err != 0) {
		LOG_ERR("Advertising start failed (%d)", err);
		return err;
	}

#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
	(void)k_work_reschedule(&g_kerfur_rotate_work,
				K_SECONDS(CONFIG_KERFUR_NEARBY_ID_ROTATE_S));
	(void)k_work_reschedule(&g_kerfur_scan_start_work, K_MSEC(500));
#endif

	LOG_INF("BLE manager ready (connectable-undirected advertising)");

	return 0;
}
