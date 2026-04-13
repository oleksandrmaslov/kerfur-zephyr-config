#ifndef KERFUR_NEARBY_H_
#define KERFUR_NEARBY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "behavior/pet_state.h"

/* Kerfur manufacturer data beacon (scan response payload). */
#define KERFUR_BEACON_MAGIC_0 'K'
#define KERFUR_BEACON_MAGIC_1 'F'
#define KERFUR_BEACON_MAGIC_2 'R'
#define KERFUR_BEACON_VERSION 0x01U

/* Using an experimental company id (RFU) until a real one is assigned. */
#define KERFUR_BEACON_COMPANY_ID 0xFFFFU

enum kerfur_social_event {
	KFR_SOCIAL_NONE = 0,
	KFR_SOCIAL_GREET,
	KFR_SOCIAL_GREET_ACK,
	KFR_SOCIAL_PLAY_INVITE,
	KFR_SOCIAL_PLAY_ACK,
	KFR_SOCIAL_BYE,
};

#define KFR_STATUS_WALKING        BIT(0)
#define KFR_STATUS_CHARGING       BIT(1)
#define KFR_STATUS_LOW_BATT       BIT(2)
#define KFR_STATUS_ASLEEP         BIT(3)
#define KFR_STATUS_BUSY           BIT(4)
#define KFR_STATUS_SOCIAL_OVERLOAD BIT(5)

struct __packed kerfur_beacon_v1 {
	uint16_t company_id;
	uint8_t  magic[3];
	uint8_t  version;
	uint8_t  ephemeral_id[4];
	uint8_t  character_id;
	uint8_t  mode_expr;       /* (mode<<4)|(expr & 0x0F) */
	uint8_t  status_flags;
	uint8_t  social_event;
	uint8_t  sequence;
};

enum kerfur_peer_state {
	KERFUR_PEER_STATE_NONE = 0,
	KERFUR_PEER_STATE_SEEN,
	KERFUR_PEER_STATE_NEAR,
	KERFUR_PEER_STATE_INTERACTING,
	KERFUR_PEER_STATE_COOLDOWN,
};

struct kerfur_peer {
	bool     in_use;
	uint32_t ephemeral_id;
	int64_t  first_seen_ms;
	int64_t  last_seen_ms;
	int16_t  rssi_avg_x16;
	int8_t   rssi_max;
	uint16_t seen_count;
	enum kerfur_peer_state state;
	uint32_t encounter_id;
	int64_t  cooldown_until_ms;
	int64_t  near_since_ms;
	int64_t  low_rssi_since_ms;
	uint8_t  character_id;
	uint8_t  mode_summary;
	uint8_t  expression_summary;
	uint8_t  status_flags;
	uint8_t  last_social_event;
	uint8_t  last_sequence;
	bool     reaction_triggered;
	bool     logged;
	bool     is_friend;
	bool     play_invite_sent;
	bool     play_ack_received;
};

struct kerfur_scan_candidate {
	uint32_t ephemeral_id;
	int8_t   rssi;
	uint8_t  character_id;
	uint8_t  mode_expr;
	uint8_t  status_flags;
	uint8_t  social_event;
	uint8_t  sequence;
	int64_t  timestamp_ms;
};

/* Snapshot of pet_state updated from main thread, read by scan/beacon builders. */
struct kerfur_pet_snapshot {
	uint8_t  mode;             /* enum pet_mode */
	uint8_t  expression;       /* enum pet_expression */
	uint8_t  status_flags;     /* KFR_STATUS_* */
	uint8_t  character_id;
	bool     battery_critical;
	bool     battery_low;
	bool     charging;
	bool     walking_active;
	bool     social_overload;
};

int  kerfur_nearby_init(void);
void kerfur_nearby_on_pet_tick(const struct pet_state *state);
void kerfur_nearby_ingest_candidate(const struct kerfur_scan_candidate *candidate);
void kerfur_nearby_tick(int64_t now_ms);

/* Parses a scan-response manufacturer data blob. Returns true on success. */
bool kerfur_nearby_parse_beacon(const uint8_t *data, size_t len,
				struct kerfur_scan_candidate *out);

/* Builds the manufacturer data for the scan response. Returns bytes written. */
size_t kerfur_nearby_build_beacon(uint8_t *out, size_t max_len);

/* Called by the BLE manager to learn the currently-used ephemeral id bytes. */
void kerfur_nearby_rotate_ephemeral_id(int64_t now_ms);

/* Friend list helpers. */
void kerfur_nearby_set_friend(uint32_t ephemeral_id, bool is_friend);
bool kerfur_nearby_is_friend(uint32_t ephemeral_id);

/* Debug / shell. */
size_t kerfur_nearby_dump_peers(struct kerfur_peer *out, size_t max);
void   kerfur_nearby_get_snapshot(struct kerfur_pet_snapshot *out);

/* Returns count of peers currently in NEAR or INTERACTING state. */
size_t kerfur_nearby_active_peer_count(void);

#endif /* KERFUR_NEARBY_H_ */
