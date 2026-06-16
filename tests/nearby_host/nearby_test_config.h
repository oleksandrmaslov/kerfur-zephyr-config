/* Kconfig values the nearby module + encounter log expect, supplied to the
 * host test via -include. Mirrors the Kconfig defaults (MAX_FRIENDS reduced
 * from 64 to keep the per-friend arrays small; the logic is identical). */
#ifndef KERFUR_NEARBY_TEST_CONFIG_H_
#define KERFUR_NEARBY_TEST_CONFIG_H_

#define CONFIG_LOG_DEFAULT_LEVEL              0

#define CONFIG_KERFUR_NEARBY_PEER_TABLE_SIZE  8
#define CONFIG_KERFUR_NEARBY_MAX_FRIENDS      8
#define CONFIG_KERFUR_NEARBY_RSSI_NEAR_DBM    (-75)
#define CONFIG_KERFUR_NEARBY_RSSI_LOST_DBM    (-90)
#define CONFIG_KERFUR_NEARBY_INTERACT_HOLD_MS 3000
#define CONFIG_KERFUR_NEARBY_COOLDOWN_MS      60000
#define CONFIG_KERFUR_NEARBY_ID_ROTATE_S      900
#define CONFIG_KERFUR_NEARBY_ENCOUNTER_LOG_SIZE 16

#endif /* KERFUR_NEARBY_TEST_CONFIG_H_ */
