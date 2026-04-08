#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"
#include "core/event_bus.h"
#include "nearby/encounter_log.h"
#include "nearby/kerfur_nearby.h"

LOG_MODULE_REGISTER(kerfur_nearby, CONFIG_LOG_DEFAULT_LEVEL);

#define PEER_TABLE_SIZE     CONFIG_KERFUR_NEARBY_PEER_TABLE_SIZE
#define RSSI_NEAR_DBM       CONFIG_KERFUR_NEARBY_RSSI_NEAR_DBM
#define RSSI_LOST_DBM       CONFIG_KERFUR_NEARBY_RSSI_LOST_DBM
#define INTERACT_HOLD_MS    CONFIG_KERFUR_NEARBY_INTERACT_HOLD_MS
#define COOLDOWN_MS         CONFIG_KERFUR_NEARBY_COOLDOWN_MS
#define ID_ROTATE_S         CONFIG_KERFUR_NEARBY_ID_ROTATE_S

#define PEER_SEEN_WINDOW_MS 8000
#define PEER_LOST_IDLE_MS   12000
#define ENCOUNTER_IDLE_MS   15000
#define ENCOUNTER_LOW_RSSI_MS 4000
#define FRIEND_TABLE_SIZE   8

#define DEVICE_SECRET_LEN   32
#define SETTINGS_SUBTREE    "kerfur/nearby"
#define SETTINGS_KEY_SECRET "secret"

struct friend_entry {
	bool     in_use;
	uint32_t ephemeral_id;
};

static K_MUTEX_DEFINE(g_peer_mutex);
static struct kerfur_peer g_peers[PEER_TABLE_SIZE];

static K_MUTEX_DEFINE(g_friend_mutex);
static struct friend_entry g_friends[FRIEND_TABLE_SIZE];

static K_MUTEX_DEFINE(g_snapshot_mutex);
static struct kerfur_pet_snapshot g_snapshot;

static K_MUTEX_DEFINE(g_secret_mutex);
static uint8_t g_device_secret[DEVICE_SECRET_LEN];
static bool g_device_secret_valid;

static uint8_t g_current_ephemeral_bytes[4];
static uint32_t g_current_ephemeral_id;
static uint8_t g_beacon_sequence;

static bool g_initialized;

/* Forward decls */
static void peer_evict_locked(int64_t now_ms);
static struct kerfur_peer *peer_find_or_alloc_locked(uint32_t ephemeral_id, int64_t now_ms);
static void peer_apply_candidate_locked(struct kerfur_peer *peer,
					const struct kerfur_scan_candidate *cand);
static void transition_to_near_locked(struct kerfur_peer *peer, int64_t now_ms);
static void transition_to_interacting_locked(struct kerfur_peer *peer, int64_t now_ms);
static void transition_to_cooldown_locked(struct kerfur_peer *peer, int64_t now_ms);
static void transition_to_none_locked(struct kerfur_peer *peer, int64_t now_ms);
static void publish_peer_event(enum app_event_type type, const struct kerfur_peer *peer,
			       int32_t duration_s);

static uint32_t crc32_ephemeral(const uint8_t *secret, int64_t time_slot)
{
	uint8_t buffer[DEVICE_SECRET_LEN + 8];
	uint64_t slot_le;

	memcpy(buffer, secret, DEVICE_SECRET_LEN);
	slot_le = sys_cpu_to_le64((uint64_t)time_slot);
	memcpy(buffer + DEVICE_SECRET_LEN, &slot_le, sizeof(slot_le));

	return crc32_ieee(buffer, sizeof(buffer));
}

static int settings_set_handler(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	const char *next;
	ssize_t rc;

	if (settings_name_steq(name, SETTINGS_KEY_SECRET, &next) && (next == NULL)) {
		if (len != DEVICE_SECRET_LEN) {
			LOG_WRN("Ignoring device_secret with unexpected len=%u",
				(unsigned)len);
			return -EINVAL;
		}

		k_mutex_lock(&g_secret_mutex, K_FOREVER);
		rc = read_cb(cb_arg, g_device_secret, DEVICE_SECRET_LEN);
		if (rc == DEVICE_SECRET_LEN) {
			g_device_secret_valid = true;
		}
		k_mutex_unlock(&g_secret_mutex);

		return (rc == DEVICE_SECRET_LEN) ? 0 : -EIO;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(kerfur_nearby, SETTINGS_SUBTREE, NULL, settings_set_handler,
			       NULL, NULL);

static int ensure_device_secret(void)
{
	int err;

	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	if (g_device_secret_valid) {
		k_mutex_unlock(&g_secret_mutex);
		return 0;
	}

	sys_rand_get(g_device_secret, DEVICE_SECRET_LEN);
	g_device_secret_valid = true;

	err = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_SECRET,
				g_device_secret, DEVICE_SECRET_LEN);
	k_mutex_unlock(&g_secret_mutex);

	if (err != 0) {
		LOG_WRN("Failed to persist device_secret (err=%d)", err);
	} else {
		LOG_INF("Generated fresh device_secret");
	}

	return err;
}

void kerfur_nearby_rotate_ephemeral_id(int64_t now_ms)
{
	int64_t time_slot;
	uint32_t id;
	uint8_t bytes[4];

	if (!g_device_secret_valid) {
		(void)ensure_device_secret();
	}

	time_slot = (now_ms / 1000) / ID_ROTATE_S;

	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	id = crc32_ephemeral(g_device_secret, time_slot);
	k_mutex_unlock(&g_secret_mutex);

	sys_put_le32(id, bytes);

	k_mutex_lock(&g_snapshot_mutex, K_FOREVER);
	memcpy(g_current_ephemeral_bytes, bytes, sizeof(bytes));
	g_current_ephemeral_id = id;
	k_mutex_unlock(&g_snapshot_mutex);

	LOG_DBG("Rotated ephemeral id to 0x%08x (slot=%lld)", id, (long long)time_slot);
}

int kerfur_nearby_init(void)
{
	int err;

	if (g_initialized) {
		return 0;
	}

	memset(g_peers, 0, sizeof(g_peers));
	memset(g_friends, 0, sizeof(g_friends));
	memset(&g_snapshot, 0, sizeof(g_snapshot));
	g_beacon_sequence = 0U;
	g_current_ephemeral_id = 0U;
	memset(g_current_ephemeral_bytes, 0, sizeof(g_current_ephemeral_bytes));

	encounter_log_init();

	err = settings_subsys_init();
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("settings_subsys_init err=%d", err);
	}
	err = settings_load_subtree(SETTINGS_SUBTREE);
	if (err != 0) {
		LOG_WRN("settings_load_subtree err=%d", err);
	}

	(void)ensure_device_secret();
	kerfur_nearby_rotate_ephemeral_id(k_uptime_get());

	g_initialized = true;
	LOG_INF("Kerfur nearby init (peer_table=%d)", PEER_TABLE_SIZE);
	return 0;
}

static uint8_t status_flags_from_state(const struct pet_state *state)
{
	uint8_t flags = 0U;

	if (state->walking_active) {
		flags |= KFR_STATUS_WALKING;
	}
	if (state->charging) {
		flags |= KFR_STATUS_CHARGING;
	}
	if (state->battery_low || state->battery_critical) {
		flags |= KFR_STATUS_LOW_BATT;
	}
	if (state->current_mode == PET_MODE_ASLEEP) {
		flags |= KFR_STATUS_ASLEEP;
	}
	if (state->current_mode == PET_MODE_INTERACTING) {
		flags |= KFR_STATUS_BUSY;
	}
	if (state->social_overload) {
		flags |= KFR_STATUS_SOCIAL_OVERLOAD;
	}
	return flags;
}

void kerfur_nearby_on_pet_tick(const struct pet_state *state)
{
	struct kerfur_pet_snapshot snap;

	if (state == NULL) {
		return;
	}

	memset(&snap, 0, sizeof(snap));
	snap.mode = (uint8_t)state->current_mode;
	snap.expression = (uint8_t)state->current_expression;
	snap.status_flags = status_flags_from_state(state);
	snap.character_id = 0U;
	snap.battery_critical = state->battery_critical;
	snap.battery_low = state->battery_low;
	snap.charging = state->charging;
	snap.walking_active = state->walking_active;
	snap.social_overload = state->social_overload;

	k_mutex_lock(&g_snapshot_mutex, K_FOREVER);
	g_snapshot = snap;
	k_mutex_unlock(&g_snapshot_mutex);
}

void kerfur_nearby_get_snapshot(struct kerfur_pet_snapshot *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_snapshot_mutex, K_FOREVER);
	*out = g_snapshot;
	k_mutex_unlock(&g_snapshot_mutex);
}

size_t kerfur_nearby_build_beacon(uint8_t *out, size_t max_len)
{
	struct kerfur_beacon_v1 beacon;
	struct kerfur_pet_snapshot snap;

	if ((out == NULL) || (max_len < sizeof(beacon))) {
		return 0U;
	}

	k_mutex_lock(&g_snapshot_mutex, K_FOREVER);
	snap = g_snapshot;
	memcpy(beacon.ephemeral_id, g_current_ephemeral_bytes, sizeof(beacon.ephemeral_id));
	k_mutex_unlock(&g_snapshot_mutex);

	beacon.company_id = sys_cpu_to_le16(KERFUR_BEACON_COMPANY_ID);
	beacon.magic[0] = KERFUR_BEACON_MAGIC_0;
	beacon.magic[1] = KERFUR_BEACON_MAGIC_1;
	beacon.magic[2] = KERFUR_BEACON_MAGIC_2;
	beacon.version = KERFUR_BEACON_VERSION;
	beacon.character_id = snap.character_id;
	beacon.mode_expr = (uint8_t)(((snap.mode & 0x0FU) << 4) | (snap.expression & 0x0FU));
	beacon.status_flags = snap.status_flags;
	beacon.social_event = (uint8_t)KFR_SOCIAL_NONE;
	beacon.sequence = g_beacon_sequence++;

	memcpy(out, &beacon, sizeof(beacon));
	return sizeof(beacon);
}

bool kerfur_nearby_parse_beacon(const uint8_t *data, size_t len,
				struct kerfur_scan_candidate *out)
{
	struct kerfur_beacon_v1 beacon;

	if ((data == NULL) || (out == NULL) || (len < sizeof(beacon))) {
		return false;
	}

	memcpy(&beacon, data, sizeof(beacon));

	if (sys_le16_to_cpu(beacon.company_id) != KERFUR_BEACON_COMPANY_ID) {
		return false;
	}
	if ((beacon.magic[0] != KERFUR_BEACON_MAGIC_0) ||
	    (beacon.magic[1] != KERFUR_BEACON_MAGIC_1) ||
	    (beacon.magic[2] != KERFUR_BEACON_MAGIC_2)) {
		return false;
	}
	if (beacon.version != KERFUR_BEACON_VERSION) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->ephemeral_id = sys_get_le32(beacon.ephemeral_id);
	out->character_id = beacon.character_id;
	out->mode_expr = beacon.mode_expr;
	out->status_flags = beacon.status_flags;
	out->social_event = beacon.social_event;
	out->sequence = beacon.sequence;
	return true;
}

/* -- Friend list ---------------------------------------------------------- */

void kerfur_nearby_set_friend(uint32_t ephemeral_id, bool is_friend)
{
	size_t i;
	ssize_t empty = -1;

	if (ephemeral_id == 0U) {
		return;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	for (i = 0U; i < FRIEND_TABLE_SIZE; i++) {
		if (g_friends[i].in_use && (g_friends[i].ephemeral_id == ephemeral_id)) {
			if (!is_friend) {
				g_friends[i].in_use = false;
				g_friends[i].ephemeral_id = 0U;
			}
			k_mutex_unlock(&g_friend_mutex);
			return;
		}
		if (!g_friends[i].in_use && (empty < 0)) {
			empty = (ssize_t)i;
		}
	}

	if (is_friend && (empty >= 0)) {
		g_friends[empty].in_use = true;
		g_friends[empty].ephemeral_id = ephemeral_id;
	}
	k_mutex_unlock(&g_friend_mutex);
}

bool kerfur_nearby_is_friend(uint32_t ephemeral_id)
{
	bool found = false;
	size_t i;

	if (ephemeral_id == 0U) {
		return false;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	for (i = 0U; i < FRIEND_TABLE_SIZE; i++) {
		if (g_friends[i].in_use && (g_friends[i].ephemeral_id == ephemeral_id)) {
			found = true;
			break;
		}
	}
	k_mutex_unlock(&g_friend_mutex);
	return found;
}

/* -- Peer table ----------------------------------------------------------- */

static struct kerfur_peer *peer_find_locked(uint32_t ephemeral_id)
{
	size_t i;

	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		if (g_peers[i].in_use && (g_peers[i].ephemeral_id == ephemeral_id)) {
			return &g_peers[i];
		}
	}
	return NULL;
}

static struct kerfur_peer *peer_find_or_alloc_locked(uint32_t ephemeral_id, int64_t now_ms)
{
	struct kerfur_peer *peer;
	size_t i;
	ssize_t oldest_slot = -1;
	int64_t oldest_ms = INT64_MAX;

	peer = peer_find_locked(ephemeral_id);
	if (peer != NULL) {
		return peer;
	}

	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		if (!g_peers[i].in_use) {
			peer = &g_peers[i];
			memset(peer, 0, sizeof(*peer));
			peer->in_use = true;
			peer->ephemeral_id = ephemeral_id;
			peer->first_seen_ms = now_ms;
			peer->last_seen_ms = now_ms;
			peer->state = KERFUR_PEER_STATE_NONE;
			peer->rssi_max = INT8_MIN;
			return peer;
		}
		if ((g_peers[i].state != KERFUR_PEER_STATE_INTERACTING) &&
		    (g_peers[i].last_seen_ms < oldest_ms)) {
			oldest_ms = g_peers[i].last_seen_ms;
			oldest_slot = (ssize_t)i;
		}
	}

	if (oldest_slot < 0) {
		return NULL;
	}

	peer = &g_peers[oldest_slot];
	memset(peer, 0, sizeof(*peer));
	peer->in_use = true;
	peer->ephemeral_id = ephemeral_id;
	peer->first_seen_ms = now_ms;
	peer->last_seen_ms = now_ms;
	peer->state = KERFUR_PEER_STATE_NONE;
	peer->rssi_max = INT8_MIN;
	return peer;
}

static void peer_evict_locked(int64_t now_ms)
{
	size_t i;

	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		struct kerfur_peer *peer = &g_peers[i];

		if (!peer->in_use) {
			continue;
		}

		if ((peer->state == KERFUR_PEER_STATE_COOLDOWN) &&
		    (now_ms >= peer->cooldown_until_ms)) {
			memset(peer, 0, sizeof(*peer));
			continue;
		}

		if ((peer->state == KERFUR_PEER_STATE_NONE) &&
		    ((now_ms - peer->last_seen_ms) > PEER_LOST_IDLE_MS)) {
			memset(peer, 0, sizeof(*peer));
			continue;
		}
	}
}

static void peer_apply_candidate_locked(struct kerfur_peer *peer,
					const struct kerfur_scan_candidate *cand)
{
	int32_t sample = (int32_t)cand->rssi * 16;
	int32_t avg;

	if (peer->seen_count == 0U) {
		peer->rssi_avg_x16 = (int16_t)sample;
	} else {
		avg = peer->rssi_avg_x16;
		avg = avg + ((sample - avg) / 4);
		peer->rssi_avg_x16 = (int16_t)avg;
	}
	if (cand->rssi > peer->rssi_max) {
		peer->rssi_max = cand->rssi;
	}
	if (peer->seen_count < UINT16_MAX) {
		peer->seen_count++;
	}
	peer->last_seen_ms = cand->timestamp_ms;
	peer->character_id = cand->character_id;
	peer->mode_summary = (cand->mode_expr >> 4) & 0x0FU;
	peer->expression_summary = cand->mode_expr & 0x0FU;
	peer->status_flags = cand->status_flags;
	peer->last_sequence = cand->sequence;
	peer->last_social_event = cand->social_event;
}

static int8_t rssi_from_avg(const struct kerfur_peer *peer)
{
	return (int8_t)(peer->rssi_avg_x16 / 16);
}

static void publish_peer_event(enum app_event_type type, const struct kerfur_peer *peer,
			       int32_t duration_s)
{
	struct app_event_peer payload = {0};

	if (peer == NULL) {
		return;
	}

	payload.ephemeral_id = peer->ephemeral_id;
	payload.encounter_id = peer->encounter_id;
	payload.rssi = rssi_from_avg(peer);
	payload.is_friend = peer->is_friend;
	payload.character_id = peer->character_id;
	payload.mode_summary = peer->mode_summary;
	payload.expression_summary = peer->expression_summary;
	payload.status_flags = peer->status_flags;
	payload.duration_s = duration_s;

	(void)app_event_publish_peer(type, &payload);
}

static void transition_to_near_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	peer->state = KERFUR_PEER_STATE_NEAR;
	peer->near_since_ms = now_ms;
	peer->low_rssi_since_ms = 0;
	LOG_INF("peer 0x%08x -> NEAR (rssi_avg=%d)", peer->ephemeral_id, rssi_from_avg(peer));
	publish_peer_event(APP_EVENT_PEER_NEAR, peer, 0);
}

static void transition_to_interacting_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	peer->state = KERFUR_PEER_STATE_INTERACTING;
	peer->encounter_id = encounter_log_begin(peer->ephemeral_id, peer->character_id, now_ms);
	peer->is_friend = kerfur_nearby_is_friend(peer->ephemeral_id);
	peer->logged = true;
	publish_peer_event(APP_EVENT_ENCOUNTER_START, peer, 0);
}

static void transition_to_cooldown_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	int32_t duration_s = 0;

	if (peer->encounter_id != 0U) {
		duration_s = (int32_t)((now_ms - peer->first_seen_ms) / 1000);
		encounter_log_end(peer->encounter_id, now_ms);
		publish_peer_event(APP_EVENT_ENCOUNTER_END, peer, duration_s);
	}

	peer->state = KERFUR_PEER_STATE_COOLDOWN;
	peer->cooldown_until_ms = now_ms + COOLDOWN_MS;
	publish_peer_event(APP_EVENT_PEER_LOST, peer, duration_s);
}

static void transition_to_none_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	ARG_UNUSED(now_ms);

	peer->state = KERFUR_PEER_STATE_NONE;
	publish_peer_event(APP_EVENT_PEER_LOST, peer, 0);
	peer->encounter_id = 0U;
}

void kerfur_nearby_ingest_candidate(const struct kerfur_scan_candidate *cand)
{
	struct kerfur_peer *peer;
	struct kerfur_pet_snapshot snap;

	if ((cand == NULL) || (cand->ephemeral_id == 0U)) {
		return;
	}

	/* Reject own beacon echo (multi-path, test injections). */
	if (cand->ephemeral_id == g_current_ephemeral_id) {
		return;
	}

	kerfur_nearby_get_snapshot(&snap);

	k_mutex_lock(&g_peer_mutex, K_FOREVER);

	peer = peer_find_or_alloc_locked(cand->ephemeral_id, cand->timestamp_ms);
	if (peer == NULL) {
		k_mutex_unlock(&g_peer_mutex);
		return;
	}

	if (peer->state == KERFUR_PEER_STATE_COOLDOWN) {
		/* Absorb packets but do not act until cooldown expires. */
		peer->last_seen_ms = cand->timestamp_ms;
		k_mutex_unlock(&g_peer_mutex);
		return;
	}

	peer_apply_candidate_locked(peer, cand);

	if (peer->state == KERFUR_PEER_STATE_NONE) {
		peer->state = KERFUR_PEER_STATE_SEEN;
		LOG_INF("peer 0x%08x first SEEN (rssi=%d)", peer->ephemeral_id, cand->rssi);
		publish_peer_event(APP_EVENT_PEER_SEEN, peer, 0);
	}

	if (peer->state == KERFUR_PEER_STATE_SEEN) {
		int8_t rssi = rssi_from_avg(peer);

		if ((peer->seen_count >= 2U) &&
		    ((cand->timestamp_ms - peer->first_seen_ms) <= PEER_SEEN_WINDOW_MS) &&
		    (rssi >= RSSI_NEAR_DBM)) {
			transition_to_near_locked(peer, cand->timestamp_ms);
		} else if (peer->seen_count >= 2U) {
			publish_peer_event(APP_EVENT_PEER_CHECKING, peer, 0);
		}
	}

	if (peer->state == KERFUR_PEER_STATE_NEAR) {
		int8_t rssi = rssi_from_avg(peer);

		if (rssi >= RSSI_NEAR_DBM) {
			peer->low_rssi_since_ms = 0;
		}

		if (!snap.battery_critical &&
		    (snap.mode != (uint8_t)PET_MODE_OVERLOADED) &&
		    (snap.mode != (uint8_t)PET_MODE_LOW_POWER) &&
		    ((cand->timestamp_ms - peer->near_since_ms) >= INTERACT_HOLD_MS)) {
			transition_to_interacting_locked(peer, cand->timestamp_ms);
		}
	}

	if (peer->state == KERFUR_PEER_STATE_INTERACTING) {
		encounter_log_update(peer->encounter_id, cand->rssi);
	}

	k_mutex_unlock(&g_peer_mutex);
}

static void prune_social_overload_locked(int64_t now_ms)
{
	struct kerfur_peer *active[PEER_TABLE_SIZE];
	size_t count = 0U;
	size_t i;
	size_t j;

	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		struct kerfur_peer *peer = &g_peers[i];

		if (!peer->in_use) {
			continue;
		}
		if ((peer->state != KERFUR_PEER_STATE_NEAR) &&
		    (peer->state != KERFUR_PEER_STATE_INTERACTING)) {
			continue;
		}
		active[count++] = peer;
	}

	if (count <= 3U) {
		return;
	}

	/* Simple selection-sort by rssi_avg, descending — the weakest get pruned. */
	for (i = 0U; i < count; i++) {
		for (j = i + 1U; j < count; j++) {
			if (active[j]->rssi_avg_x16 > active[i]->rssi_avg_x16) {
				struct kerfur_peer *tmp = active[i];

				active[i] = active[j];
				active[j] = tmp;
			}
		}
	}

	/* Keep the strongest ≤3, drop the rest. */
	for (i = 3U; i < count; i++) {
		struct kerfur_peer *peer = active[i];

		if (peer->state == KERFUR_PEER_STATE_INTERACTING) {
			transition_to_cooldown_locked(peer, now_ms);
		} else {
			transition_to_none_locked(peer, now_ms);
		}
	}
}

void kerfur_nearby_tick(int64_t now_ms)
{
	size_t i;

	k_mutex_lock(&g_peer_mutex, K_FOREVER);

	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		struct kerfur_peer *peer = &g_peers[i];
		int8_t rssi;
		int64_t idle_ms;

		if (!peer->in_use) {
			continue;
		}

		idle_ms = now_ms - peer->last_seen_ms;

		switch (peer->state) {
		case KERFUR_PEER_STATE_NONE:
			break;
		case KERFUR_PEER_STATE_SEEN:
			if (idle_ms > PEER_SEEN_WINDOW_MS) {
				peer->state = KERFUR_PEER_STATE_NONE;
			}
			break;
		case KERFUR_PEER_STATE_NEAR:
			rssi = rssi_from_avg(peer);
			if (rssi < RSSI_LOST_DBM) {
				if (peer->low_rssi_since_ms == 0) {
					peer->low_rssi_since_ms = now_ms;
				} else if ((now_ms - peer->low_rssi_since_ms) >=
					   ENCOUNTER_LOW_RSSI_MS) {
					transition_to_none_locked(peer, now_ms);
				}
			}
			if (idle_ms > PEER_LOST_IDLE_MS) {
				transition_to_none_locked(peer, now_ms);
			}
			break;
		case KERFUR_PEER_STATE_INTERACTING:
			rssi = rssi_from_avg(peer);
			if (rssi < RSSI_LOST_DBM) {
				if (peer->low_rssi_since_ms == 0) {
					peer->low_rssi_since_ms = now_ms;
				} else if ((now_ms - peer->low_rssi_since_ms) >=
					   ENCOUNTER_LOW_RSSI_MS) {
					transition_to_cooldown_locked(peer, now_ms);
				}
			} else {
				peer->low_rssi_since_ms = 0;
			}
			if (idle_ms > ENCOUNTER_IDLE_MS) {
				transition_to_cooldown_locked(peer, now_ms);
			}
			break;
		case KERFUR_PEER_STATE_COOLDOWN:
			break;
		default:
			break;
		}
	}

	prune_social_overload_locked(now_ms);
	peer_evict_locked(now_ms);

	k_mutex_unlock(&g_peer_mutex);
}

size_t kerfur_nearby_active_peer_count(void)
{
	size_t count = 0U;
	size_t i;

	k_mutex_lock(&g_peer_mutex, K_FOREVER);
	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		if (!g_peers[i].in_use) {
			continue;
		}
		if ((g_peers[i].state == KERFUR_PEER_STATE_NEAR) ||
		    (g_peers[i].state == KERFUR_PEER_STATE_INTERACTING)) {
			count++;
		}
	}
	k_mutex_unlock(&g_peer_mutex);

	return count;
}

size_t kerfur_nearby_dump_peers(struct kerfur_peer *out, size_t max)
{
	size_t written = 0U;
	size_t i;

	if ((out == NULL) || (max == 0U)) {
		return 0U;
	}

	k_mutex_lock(&g_peer_mutex, K_FOREVER);
	for (i = 0U; (i < PEER_TABLE_SIZE) && (written < max); i++) {
		if (g_peers[i].in_use) {
			out[written++] = g_peers[i];
		}
	}
	k_mutex_unlock(&g_peer_mutex);

	return written;
}
