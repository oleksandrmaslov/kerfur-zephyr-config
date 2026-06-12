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
	/* Stable relationship identity: index into the persistent friend table.
	 * -1 means this peer is not a confirmed friend.  Survives ephemeral ID
	 * rotation within a session (nearby module updates ephemeral_id but keeps
	 * this index) so the behavior engine always sees the right context. */
	int8_t   friend_index;
	/* How many complete encounters happened with this peer in the current
	 * power cycle.  0 = first meeting (stranger); ≥1 = "familiar" — we have
	 * met before this session.  Survives ephemeral ID rotation.  The peer
	 * entry lingers after cooldown (in NONE state) so this context is
	 * preserved across brief separations within a session. */
	uint8_t  session_encounters;
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

/* ── Persistent friend record ──────────────────────────────────────────────
 *
 * Friends are recognized across reboots and ephemeral ID rotation via an
 * IRK-style mechanism: confirmed friends share their device_secret (32 bytes)
 * through the companion app.  The nearby module uses that key to predict the
 * friend's ephemeral ID for any time slot, enabling autonomous recognition
 * without any hidden tracking.
 *
 * Recognition requires both devices to have wall-clock time synced via the
 * companion app (APP_EVENT_TIME_SYNC), so that their time slots align.
 *
 * Privacy guarantee: the friend_key is only stored on Kerfus; it is never
 * broadcast over the air.  Ephemeral IDs remain the only over-the-air ID. */

#define KERFUR_FRIEND_KEY_LEN      32   /* == device_secret length */
#define KERFUR_FRIEND_NICKNAME_LEN 16   /* null-terminated; 15 chars + NUL */

struct kerfur_friend_record {
	bool     in_use;
	int8_t   friend_index;                       /* slot [0..MAX_FRIENDS) */
	uint8_t  peer_key[KERFUR_FRIEND_KEY_LEN];   /* peer device_secret */
	char     nickname[KERFUR_FRIEND_NICKNAME_LEN];
	uint32_t encounter_count;                    /* NVS-persisted */
	int64_t  last_seen_uptime_ms;                /* session-only */
};

/* ── Core API ──────────────────────────────────────────────────────────── */

int  kerfur_nearby_init(void);
void kerfur_nearby_on_pet_tick(const struct pet_state *state);
void kerfur_nearby_ingest_candidate(const struct kerfur_scan_candidate *candidate);
void kerfur_nearby_tick(int64_t now_ms);

/* Parses a scan-response manufacturer data blob. Returns true on success. */
bool kerfur_nearby_parse_beacon(const uint8_t *data, size_t len,
				struct kerfur_scan_candidate *out);

/* Builds the manufacturer data for the scan response. Returns bytes written. */
size_t kerfur_nearby_build_beacon(uint8_t *out, size_t max_len);

/* Called by the BLE manager to update the ephemeral ID.  When wall clock is
 * valid the ID uses the wall-clock slot so friends can predict it; otherwise
 * it falls back to the uptime slot (private but not cross-device resolvable). */
void kerfur_nearby_rotate_ephemeral_id(int64_t now_ms);

/* Provide a wall-clock reference so friend resolution can align time slots.
 * unix_s  : current Unix timestamp (seconds since epoch, must be >= 946684800).
 * uptime_ms: k_uptime_get() value recorded at the same moment as unix_s.
 * Triggers an immediate ephemeral ID rotation to the new wall-clock slot. */
void kerfur_nearby_set_wall_clock(int64_t unix_s, int64_t uptime_ms);

/* ── Friend management API ─────────────────────────────────────────────── */

/* Add or update a confirmed friend.  peer_key must be KERFUR_FRIEND_KEY_LEN
 * bytes (the friend's device_secret, shared via companion app).  nickname is
 * optional (may be NULL).  Returns the slot index [0..MAX_FRIENDS) on success,
 * negative errno on failure (-ENOMEM = table full, -EINVAL = bad key). */
int kerfur_nearby_add_friend(const uint8_t *peer_key, const char *nickname,
			     size_t nickname_len);

/* Remove a confirmed friend by slot index.  Zeroes the NVS slot (tombstone).
 * Returns 0 on success, -ENOENT if slot is not in use, -EINVAL if index bad. */
int kerfur_nearby_remove_friend(uint8_t friend_index);

/* Copy up to max records into out.  Returns number of records written.
 * in_use slots only; friend_index field is filled with the slot position. */
size_t kerfur_nearby_dump_friends(struct kerfur_friend_record *out, size_t max);

/* Compute the current expected ephemeral ID for a stored friend (uses the
 * wall-clock slot; returns 0 if wall clock not valid or index invalid). */
uint32_t kerfur_nearby_friend_expected_id(uint8_t friend_index);

/* Resolve an ephemeral ID to a friend slot.  Returns the friend_index
 * [0..MAX_FRIENDS) if the ID matches any stored friend for the current slot
 * (±1 for boundary tolerance), or -1 if unknown or wall clock not valid. */
int8_t kerfur_nearby_resolve_ephemeral_id(uint32_t ephemeral_id);

/* ── Debug / shell ─────────────────────────────────────────────────────── */

/* Returns count of peers currently in NEAR or INTERACTING state. */
size_t kerfur_nearby_active_peer_count(void);

size_t kerfur_nearby_dump_peers(struct kerfur_peer *out, size_t max);
void   kerfur_nearby_get_snapshot(struct kerfur_pet_snapshot *out);

#endif /* KERFUR_NEARBY_H_ */
