#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ble/ble_manager.h"
#include "core/event_bus.h"

LOG_MODULE_REGISTER(ble_manager, CONFIG_LOG_DEFAULT_LEVEL);

#define BT_UUID_KERFUR_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x4a7b0001, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)
#define BT_UUID_KERFUR_EVENT_INJECT_VAL \
	BT_UUID_128_ENCODE(0x4a7b0002, 0x1e1a, 0x4d52, 0xa2ef, 0x14bb5f420001)

static struct bt_uuid_128 g_kerfur_service_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_SERVICE_VAL);
static struct bt_uuid_128 g_kerfur_event_inject_uuid = BT_UUID_INIT_128(BT_UUID_KERFUR_EVENT_INJECT_VAL);
static const uint8_t g_adv_flags[] = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
static const struct bt_data g_adv_data[] = {
	BT_DATA(BT_DATA_FLAGS, g_adv_flags, sizeof(g_adv_flags)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};

static enum app_event_type decode_injected_event(uint8_t code)
{
	switch (code) {
	case 1:
		return APP_EVENT_MOCK_PET;
	case 2:
		return APP_EVENT_MOCK_SHAKE;
	case 3:
		return APP_EVENT_MOCK_NOTIFICATION;
	case 4:
		return APP_EVENT_SLEEP_REQUEST;
	case 5:
		return APP_EVENT_WAKE;
	default:
		return APP_EVENT_COUNT;
	}
}

static ssize_t event_inject_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	enum app_event_type type;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	type = decode_injected_event(((const uint8_t *)buf)[0]);
	if (type == APP_EVENT_COUNT) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	LOG_INF("BLE inject: code=%u -> %s", ((const uint8_t *)buf)[0], app_event_type_str(type));
	(void)app_event_publish(type, 0);
	return len;
}

BT_GATT_SERVICE_DEFINE(kerfur_svc,
	BT_GATT_PRIMARY_SERVICE(&g_kerfur_service_uuid),
	BT_GATT_CHARACTERISTIC(&g_kerfur_event_inject_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, event_inject_write_cb, NULL),
);

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err == 0U) {
		(void)app_event_publish(APP_EVENT_BLE_CONNECTED, 0);
		LOG_INF("BLE connected");
	}
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	(void)app_event_publish(APP_EVENT_BLE_DISCONNECTED, reason);
	LOG_INF("BLE disconnected (reason 0x%02x)", reason);
}

BT_CONN_CB_DEFINE(g_bt_conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

int ble_manager_init(void)
{
	int err;

	if (!IS_ENABLED(CONFIG_KERFUR_ENABLE_BLE)) {
		LOG_INF("BLE scaffold disabled by Kconfig");
		return 0;
	}

	/* TODO: Extend toward companion-app bridge and notification transport. */
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, g_adv_data, ARRAY_SIZE(g_adv_data), NULL, 0);
	if (err) {
		LOG_ERR("Advertising start failed (%d)", err);
		return err;
	}

	LOG_INF("BLE debug service ready. Write 1..5 to inject events");
	return 0;
}
