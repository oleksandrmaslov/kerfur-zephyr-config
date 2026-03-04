#ifndef KERFUR_BLE_MANAGER_H_
#define KERFUR_BLE_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

int ble_manager_init(void);
int ble_manager_restart_advertising(void);
int ble_manager_disconnect_current(void);
int ble_manager_unpair_all(void);
bool ble_manager_is_connected(void);
bool ble_manager_is_companion_subscribed(void);
int ble_manager_companion_send_test_notification(uint8_t category_id);
int ble_manager_rsc_notify(uint16_t speed_256ms, uint8_t cadence_spm, bool running);

#endif /* KERFUR_BLE_MANAGER_H_ */
