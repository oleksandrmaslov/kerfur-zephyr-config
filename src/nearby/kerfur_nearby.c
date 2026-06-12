#include <errno.h>
#include <stdio.h>
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
#define MAX_FRIENDS         CONFIG_KERFUR_NEARBY_MAX_FRIENDS
#define RSSI_NEAR_DBM       CONFIG_KERFUR_NEARBY_RSSI_NEAR_DBM
#define RSSI_LOST_DBM       CONFIG_KERFUR_NEARBY_RSSI_LOST_DBM
#define INTERACT_HOLD_MS    CONFIG_KERFUR_NEARBY_INTERACT_HOLD_MS
#define COOLDOWN_MS         CONFIG_KERFUR_NEARBY_COOLDOWN_MS
#define ID_ROTATE_S         CONFIG_KERFUR_NEARBY_ID_ROTATE_S

#define PEER_SEEN_WINDOW_MS        8000
#define PEER_LOST_IDLE_MS          12000
/* Familiar peers (session_encounters > 0) linger in NONE state longer so their
 * session context survives brief separations between encounter cycles. */
#define PEER_FAMILIAR_LINGER_MS    (5 * 60 * 1000LL)
#define ENCOUNTER_IDLE_MS          15000
#define ENCOUNTER_LOW_RSSI_MS      4000
/* Max simultaneous NEAR/INTERACTING peers.  Excess (weakest RSSI) are evicted
 * to protect the emotion engine from being overwhelmed; this is a resource cap,
 * NOT the social-overload signal (which comes from the emotion engine's drives). */
#define MAX_ACTIVE_ENCOUNTERS      5

#define DEVICE_SECRET_LEN   32
#define SETTINGS_SUBTREE    "kerfur/nearby"
#define SETTINGS_KEY_SECRET "secret"
#define SETTINGS_KEY_FRIEND "f"   /* friend slots live at kerfur/nearby/f/N */

/* ── Friend NVS blob (persisted layout — do not reorder fields) ─────── */

struct friend_nvm_blob {
	uint8_t  peer_key[KERFUR_FRIEND_KEY_LEN];
	char     nickname[KERFUR_FRIEND_NICKNAME_LEN];
	uint32_t encounter_count;
};

/* All-zero key used as tombstone to erase a friend slot. */
static const uint8_t g_zero_key[KERFUR_FRIEND_KEY_LEN]; /* BSS, always 0 */

/* ── File-local state ───────────────────────────────────────────────── */

static K_MUTEX_DEFINE(g_peer_mutex);
static struct kerfur_peer g_peers[PEER_TABLE_SIZE];

static K_MUTEX_DEFINE(g_friend_mutex);
/* Internal friend table — mirrors kerfur_friend_record without friend_index
 * (that is implicit from the array position). */
static struct kerfur_friend_record g_friends[MAX_FRIENDS];

/* Per-slot ID cache: recomputed once per wall-clock rotation (~15 min), not on
 * every incoming beacon.  Hot-path resolution becomes 3 integer compares per
 * friend instead of 3 CRC32 calls — O(n) comparisons vs O(n * 3 * CRC32).
 * Protected by g_friend_mutex; slot = INT64_MIN means "not yet computed." */
struct friend_id_cache {
	int64_t  slot;   /* wall-clock slot these were computed for */
	uint32_t id[3];  /* expected IDs for slot-1, slot, slot+1
			  * (0 stored for negative/invalid slots) */
};
static struct friend_id_cache g_friend_cache[MAX_FRIENDS];

static K_MUTEX_DEFINE(g_snapshot_mutex);
static struct kerfur_pet_snapshot g_snapshot;

/* g_secret_mutex also guards the wall-clock reference. */
static K_MUTEX_DEFINE(g_secret_mutex);
static uint8_t  g_device_secret[DEVICE_SECRET_LEN];
static bool     g_device_secret_valid;
static bool     g_wallclock_valid;
static int64_t  g_wallclock_unix_s;   /* unix time at sync point */
static int64_t  g_wallclock_uptime_ms;/* uptime_ms recorded alongside unix_s */

static uint8_t  g_current_ephemeral_bytes[4];
static uint32_t g_current_ephemeral_id;
static uint8_t  g_beacon_sequence;

static bool g_initialized;

/* Pending friend encounter-count update: filled by transition_to_cooldown_locked()
 * while peer_mutex is held; flushed by kerfur_nearby_tick() after releasing it.
 * Bit N = friend slot N completed an encounter this tick.
 * uint64_t supports MAX_FRIENDS up to 64 (== KERFUR_NEARBY_MAX_FRIENDS max). */
static uint64_t g_pending_friend_count_mask;
static int64_t  g_pending_friend_count_ms;

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

/* ── CRC32 ephemeral ID computation ─────────────────────────────────── */

static uint32_t crc32_ephemeral(const uint8_t *secret, int64_t time_slot)
{
	uint8_t buffer[DEVICE_SECRET_LEN + 8];
	uint64_t slot_le;

	memcpy(buffer, secret, DEVICE_SECRET_LEN);
	slot_le = sys_cpu_to_le64((uint64_t)time_slot);
	memcpy(buffer + DEVICE_SECRET_LEN, &slot_le, sizeof(slot_le));

	return crc32_ieee(buffer, sizeof(buffer));
}

/* Compute the wall-clock time slot for a given uptime value.
 * Returns the uptime-based slot when wall clock is not valid. */
static int64_t current_time_slot(int64_t now_ms)
{
	if (g_wallclock_valid) {
		int64_t elapsed_ms = now_ms - g_wallclock_uptime_ms;
		int64_t unix_s = g_wallclock_unix_s + (elapsed_ms / 1000);

		return unix_s / (int64_t)ID_ROTATE_S;
	}
	return (now_ms / 1000) / (int64_t)ID_ROTATE_S;
}

/* ── Settings ────────────────────────────────────────────────────────── */

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

	/* Friend slots: kerfur/nearby/f/N  (N = 0 .. MAX_FRIENDS-1) */
	if (settings_name_steq(name, SETTINGS_KEY_FRIEND, &next) && (next != NULL)) {
		unsigned long idx = strtoul(next, NULL, 10);

		if (idx >= (unsigned long)MAX_FRIENDS) {
			return -ENOENT;
		}
		if (len != sizeof(struct friend_nvm_blob)) {
			LOG_WRN("Ignoring friend/%lu with unexpected len=%u",
				idx, (unsigned)len);
			return -EINVAL;
		}

		struct friend_nvm_blob blob;

		rc = read_cb(cb_arg, &blob, sizeof(blob));
		if (rc != sizeof(blob)) {
			return -EIO;
		}

		k_mutex_lock(&g_friend_mutex, K_FOREVER);
		if (memcmp(blob.peer_key, g_zero_key, KERFUR_FRIEND_KEY_LEN) == 0) {
			/* Tombstone — slot was explicitly removed. */
			memset(&g_friends[idx], 0, sizeof(g_friends[idx]));
		} else {
			g_friends[idx].in_use = true;
			g_friends[idx].friend_index = (int8_t)idx;
			memcpy(g_friends[idx].peer_key, blob.peer_key,
			       KERFUR_FRIEND_KEY_LEN);
			blob.nickname[KERFUR_FRIEND_NICKNAME_LEN - 1] = '\0';
			memcpy(g_friends[idx].nickname, blob.nickname,
			       KERFUR_FRIEND_NICKNAME_LEN);
			g_friends[idx].encounter_count = blob.encounter_count;
			g_friends[idx].last_seen_uptime_ms = 0;
		}
		k_mutex_unlock(&g_friend_mutex);

		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(kerfur_nearby, SETTINGS_SUBTREE, NULL, settings_set_handler,
				NULL, NULL);

/* ── NVS helpers ─────────────────────────────────────────────────────── */

static void save_friend(uint8_t idx)
{
	struct friend_nvm_blob blob;
	char key_path[40];
	int err;

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	if (idx >= (uint8_t)MAX_FRIENDS) {
		k_mutex_unlock(&g_friend_mutex);
		return;
	}
	memcpy(blob.peer_key, g_friends[idx].peer_key, KERFUR_FRIEND_KEY_LEN);
	memcpy(blob.nickname, g_friends[idx].nickname, KERFUR_FRIEND_NICKNAME_LEN);
	blob.encounter_count = g_friends[idx].encounter_count;
	k_mutex_unlock(&g_friend_mutex);

	snprintf(key_path, sizeof(key_path),
		 SETTINGS_SUBTREE "/" SETTINGS_KEY_FRIEND "/%u", (unsigned)idx);
	err = settings_save_one(key_path, &blob, sizeof(blob));
	if (err != 0) {
		LOG_WRN("Failed to save friend %u (err=%d)", idx, err);
	} else {
		LOG_DBG("Saved friend %u (count=%u)", idx, blob.encounter_count);
	}
}

static void save_friend_tombstone(uint8_t idx)
{
	struct friend_nvm_blob blob = {0};
	char key_path[40];
	int err;

	snprintf(key_path, sizeof(key_path),
		 SETTINGS_SUBTREE "/" SETTINGS_KEY_FRIEND "/%u", (unsigned)idx);
	err = settings_save_one(key_path, &blob, sizeof(blob));
	if (err != 0) {
		LOG_WRN("Failed to tombstone friend %u (err=%d)", idx, err);
	}
}

/* ── Device secret ───────────────────────────────────────────────────── */

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

/* ── Ephemeral ID rotation ───────────────────────────────────────────── */

void kerfur_nearby_rotate_ephemeral_id(int64_t now_ms)
{
	int64_t time_slot;
	uint32_t id;
	uint8_t bytes[4];

	if (!g_device_secret_valid) {
		(void)ensure_device_secret();
	}

	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	time_slot = current_time_slot(now_ms);
	id = crc32_ephemeral(g_device_secret, time_slot);
	k_mutex_unlock(&g_secret_mutex);

	sys_put_le32(id, bytes);

	k_mutex_lock(&g_snapshot_mutex, K_FOREVER);
	memcpy(g_current_ephemeral_bytes, bytes, sizeof(bytes));
	g_current_ephemeral_id = id;
	k_mutex_unlock(&g_snapshot_mutex);

	LOG_DBG("Rotated ephemeral id to 0x%08x (slot=%lld, %s)",
		id, (long long)time_slot,
		g_wallclock_valid ? "wall-clock" : "uptime");
}

void kerfur_nearby_set_wall_clock(int64_t unix_s, int64_t uptime_ms)
{
	/* Reject implausible unix timestamps (pre-2000). */
	if (unix_s < 946684800LL) {
		return;
	}

	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	g_wallclock_unix_s = unix_s;
	g_wallclock_uptime_ms = uptime_ms;
	g_wallclock_valid = true;
	k_mutex_unlock(&g_secret_mutex);

	/* Invalidate all friend caches: the slot numbers are now wall-clock-based
	 * instead of uptime-based, so every cached ID is stale. */
	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	for (int j = 0; j < MAX_FRIENDS; j++) {
		g_friend_cache[j].slot = INT64_MIN;
	}
	k_mutex_unlock(&g_friend_mutex);

	/* Immediately rotate to the wall-clock-aligned slot. */
	kerfur_nearby_rotate_ephemeral_id(uptime_ms);

	LOG_INF("Wall clock synced (unix=%lld) — friend resolution enabled",
		(long long)unix_s);
}

/* ── Friend resolution (IRK-style) ──────────────────────────────────── */

/* Returns friend_index [0..MAX_FRIENDS) if ephemeral_id matches any stored
 * friend for the current wall-clock slot (±1), or -1 if no match or no clock.
 * Called from the BLE thread before g_peer_mutex is acquired.
 *
 * Performance: the per-friend ID cache is valid for one rotation period
 * (~15 min).  Hot path = 3 integer comparisons per friend slot; the 3 CRC32
 * computations happen lazily on the first call after each slot change. */
static int8_t resolve_friend_id(uint32_t ephemeral_id, int64_t now_ms)
{
	int64_t base_slot;
	bool clock_valid;
	int64_t clock_unix_s, clock_uptime_ms;
	int i;

	/* ephemeral_id == 0 is the "own beacon echo / unset" sentinel;
	 * 0 is also stored in cache entries for negative slots, so bail
	 * early to avoid a false match. */
	if (ephemeral_id == 0U) {
		return -1;
	}

	/* Snapshot wall clock state without holding friend_mutex. */
	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	clock_valid     = g_wallclock_valid;
	clock_unix_s    = g_wallclock_unix_s;
	clock_uptime_ms = g_wallclock_uptime_ms;
	k_mutex_unlock(&g_secret_mutex);

	if (!clock_valid) {
		return -1;
	}

	{
		int64_t elapsed_ms = now_ms - clock_uptime_ms;
		int64_t unix_s = clock_unix_s + (elapsed_ms / 1000);

		base_slot = unix_s / (int64_t)ID_ROTATE_S;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	for (i = 0; i < MAX_FRIENDS; i++) {
		if (!g_friends[i].in_use) {
			continue;
		}

		/* Cache miss: recompute the 3 expected IDs for this friend.
		 * Happens at most once per rotation period per slot (~15 min). */
		if (g_friend_cache[i].slot != base_slot) {
			g_friend_cache[i].id[0] = (base_slot > 0LL)
				? crc32_ephemeral(g_friends[i].peer_key, base_slot - 1LL)
				: 0U;  /* slot -1 invalid; 0 never matches real ID */
			g_friend_cache[i].id[1] =
				crc32_ephemeral(g_friends[i].peer_key, base_slot);
			g_friend_cache[i].id[2] =
				crc32_ephemeral(g_friends[i].peer_key, base_slot + 1LL);
			g_friend_cache[i].slot = base_slot;
		}

		/* Hot path: 3 integer comparisons — no CRC32. */
		if ((g_friend_cache[i].id[0] == ephemeral_id) ||
		    (g_friend_cache[i].id[1] == ephemeral_id) ||
		    (g_friend_cache[i].id[2] == ephemeral_id)) {
			k_mutex_unlock(&g_friend_mutex);
			return (int8_t)i;
		}
	}
	k_mutex_unlock(&g_friend_mutex);
	return -1;
}

/* ── Public friend API ───────────────────────────────────────────────── */

int kerfur_nearby_add_friend(const uint8_t *peer_key, const char *nickname,
			     size_t nickname_len)
{
	int i;
	ssize_t empty_slot = -1;

	if ((peer_key == NULL) ||
	    (memcmp(peer_key, g_zero_key, KERFUR_FRIEND_KEY_LEN) == 0)) {
		return -EINVAL;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);

	for (i = 0; i < MAX_FRIENDS; i++) {
		if (g_friends[i].in_use) {
			if (memcmp(g_friends[i].peer_key, peer_key,
				   KERFUR_FRIEND_KEY_LEN) == 0) {
				/* Already known — update nickname if provided. */
				if ((nickname != NULL) && (nickname_len > 0U)) {
					size_t copy = MIN(nickname_len,
							  KERFUR_FRIEND_NICKNAME_LEN - 1U);

					memcpy(g_friends[i].nickname, nickname, copy);
					g_friends[i].nickname[copy] = '\0';
				}
				k_mutex_unlock(&g_friend_mutex);
				save_friend((uint8_t)i);
				return i;
			}
		} else if (empty_slot < 0) {
			empty_slot = (ssize_t)i;
		}
	}

	if (empty_slot < 0) {
		k_mutex_unlock(&g_friend_mutex);
		return -ENOMEM;
	}

	g_friends[empty_slot].in_use = true;
	g_friends[empty_slot].friend_index = (int8_t)empty_slot;
	memcpy(g_friends[empty_slot].peer_key, peer_key, KERFUR_FRIEND_KEY_LEN);
	memset(g_friends[empty_slot].nickname, 0, KERFUR_FRIEND_NICKNAME_LEN);
	if ((nickname != NULL) && (nickname_len > 0U)) {
		size_t copy = MIN(nickname_len, KERFUR_FRIEND_NICKNAME_LEN - 1U);

		memcpy(g_friends[empty_slot].nickname, nickname, copy);
		g_friends[empty_slot].nickname[copy] = '\0';
	}
	g_friends[empty_slot].encounter_count = 0U;
	g_friends[empty_slot].last_seen_uptime_ms = 0;

	/* Invalidate cache so the new peer_key is picked up immediately. */
	g_friend_cache[empty_slot].slot = INT64_MIN;

	k_mutex_unlock(&g_friend_mutex);
	save_friend((uint8_t)empty_slot);

	LOG_INF("Friend added: slot=%ld name=%s", (long)empty_slot,
		g_friends[empty_slot].nickname[0]
			? g_friends[empty_slot].nickname
			: "(unnamed)");
	return (int)empty_slot;
}

int kerfur_nearby_remove_friend(uint8_t friend_index)
{
	if (friend_index >= (uint8_t)MAX_FRIENDS) {
		return -EINVAL;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	if (!g_friends[friend_index].in_use) {
		k_mutex_unlock(&g_friend_mutex);
		return -ENOENT;
	}
	memset(&g_friends[friend_index], 0, sizeof(g_friends[friend_index]));
	memset(&g_friend_cache[friend_index], 0, sizeof(g_friend_cache[friend_index]));
	g_friend_cache[friend_index].slot = INT64_MIN;
	k_mutex_unlock(&g_friend_mutex);

	save_friend_tombstone(friend_index);
	LOG_INF("Friend removed: slot=%u", friend_index);
	return 0;
}

size_t kerfur_nearby_dump_friends(struct kerfur_friend_record *out, size_t max)
{
	size_t written = 0U;
	size_t i;

	if ((out == NULL) || (max == 0U)) {
		return 0U;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	for (i = 0U; (i < (size_t)MAX_FRIENDS) && (written < max); i++) {
		if (g_friends[i].in_use) {
			out[written] = g_friends[i];
			out[written].friend_index = (int8_t)i;
			written++;
		}
	}
	k_mutex_unlock(&g_friend_mutex);

	return written;
}

uint32_t kerfur_nearby_friend_expected_id(uint8_t friend_index)
{
	int64_t now_ms = k_uptime_get();
	int64_t slot;
	uint32_t expected = 0U;
	bool clock_valid;
	int64_t clock_unix_s, clock_uptime_ms;

	if (friend_index >= (uint8_t)MAX_FRIENDS) {
		return 0U;
	}

	k_mutex_lock(&g_secret_mutex, K_FOREVER);
	clock_valid     = g_wallclock_valid;
	clock_unix_s    = g_wallclock_unix_s;
	clock_uptime_ms = g_wallclock_uptime_ms;
	k_mutex_unlock(&g_secret_mutex);

	if (!clock_valid) {
		return 0U;
	}

	{
		int64_t elapsed_ms = now_ms - clock_uptime_ms;
		int64_t unix_s = clock_unix_s + (elapsed_ms / 1000);

		slot = unix_s / (int64_t)ID_ROTATE_S;
	}

	k_mutex_lock(&g_friend_mutex, K_FOREVER);
	if (g_friends[friend_index].in_use) {
		expected = crc32_ephemeral(g_friends[friend_index].peer_key, slot);
	}
	k_mutex_unlock(&g_friend_mutex);

	return expected;
}

int8_t kerfur_nearby_resolve_ephemeral_id(uint32_t ephemeral_id)
{
	if (ephemeral_id == 0U) {
		return -1;
	}
	return resolve_friend_id(ephemeral_id, k_uptime_get());
}

/* ── Init ────────────────────────────────────────────────────────────── */

int kerfur_nearby_init(void)
{
	int err;

	if (g_initialized) {
		return 0;
	}

	memset(g_peers, 0, sizeof(g_peers));
	memset(g_friends, 0, sizeof(g_friends));
	memset(g_friend_cache, 0, sizeof(g_friend_cache));
	/* BSS zero is a valid slot number in theory (unix epoch / ID_ROTATE_S),
	 * though extremely unlikely for 2026-era devices.  Be explicit. */
	for (int i = 0; i < MAX_FRIENDS; i++) {
		g_friend_cache[i].slot = INT64_MIN;
	}
	memset(&g_snapshot, 0, sizeof(g_snapshot));
	g_beacon_sequence = 0U;
	g_current_ephemeral_id = 0U;
	memset(g_current_ephemeral_bytes, 0, sizeof(g_current_ephemeral_bytes));
	g_pending_friend_count_mask = 0U;
	g_pending_friend_count_ms = 0;

	encounter_log_init();

	err = settings_subsys_init();
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("settings_subsys_init err=%d", err);
	}
	err = settings_load_subtree(SETTINGS_SUBTREE);
	if (err != 0) {
		LOG_WRN("settings_load_subtree err=%d", err);
	}

	/* Log how many friends were restored from NVS. */
	{
		int friend_count = 0;

		for (int i = 0; i < MAX_FRIENDS; i++) {
			if (g_friends[i].in_use) {
				g_friends[i].friend_index = (int8_t)i;
				friend_count++;
				LOG_INF("Restored friend slot %d: \"%s\" (%u encounters)",
					i,
					g_friends[i].nickname[0]
						? g_friends[i].nickname
						: "(unnamed)",
					g_friends[i].encounter_count);
			}
		}
		if (friend_count == 0) {
			LOG_INF("No confirmed friends stored");
		}
	}

	(void)ensure_device_secret();
	kerfur_nearby_rotate_ephemeral_id(k_uptime_get());

	g_initialized = true;
	LOG_INF("Kerfur nearby init (peer_table=%d, max_friends=%d)",
		PEER_TABLE_SIZE, MAX_FRIENDS);
	return 0;
}

/* ── Snapshot ────────────────────────────────────────────────────────── */

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

/* ── Beacon build / parse ────────────────────────────────────────────── */

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

/* ── Peer table helpers ──────────────────────────────────────────────── */

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

/* Find an existing peer that belongs to a specific confirmed friend slot.
 * Used to handle mid-encounter ephemeral ID rotation transparently. */
static struct kerfur_peer *peer_find_friend_locked(int8_t friend_index)
{
	size_t i;

	if (friend_index < 0) {
		return NULL;
	}
	for (i = 0U; i < PEER_TABLE_SIZE; i++) {
		if (g_peers[i].in_use && (g_peers[i].friend_index == friend_index)) {
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
			peer->friend_index = -1;  /* resolved on first ingest */
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
	peer->friend_index = -1;
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

		/* When cooldown expires, downgrade to NONE rather than evicting
		 * immediately.  This preserves session_encounters so a familiar
		 * peer that comes back shortly is still recognized as "known." */
		if ((peer->state == KERFUR_PEER_STATE_COOLDOWN) &&
		    (now_ms >= peer->cooldown_until_ms)) {
			peer->state = KERFUR_PEER_STATE_NONE;
			peer->encounter_id = 0U;
			continue;
		}

		if (peer->state == KERFUR_PEER_STATE_NONE) {
			int64_t idle_ms = now_ms - peer->last_seen_ms;
			/* Familiar peers linger much longer to preserve session
			 * context across brief separations between encounters. */
			int64_t linger_ms = (peer->session_encounters > 0U)
					    ? PEER_FAMILIAR_LINGER_MS
					    : PEER_LOST_IDLE_MS;

			if (idle_ms > linger_ms) {
				memset(peer, 0, sizeof(*peer));
			}
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

	payload.ephemeral_id      = peer->ephemeral_id;
	payload.encounter_id      = peer->encounter_id;
	payload.rssi              = rssi_from_avg(peer);
	payload.is_friend         = peer->is_friend;
	payload.friend_index      = peer->friend_index;
	payload.session_encounters = peer->session_encounters;
	payload.character_id      = peer->character_id;
	payload.mode_summary      = peer->mode_summary;
	payload.expression_summary = peer->expression_summary;
	payload.status_flags      = peer->status_flags;
	payload.duration_s        = duration_s;

	(void)app_event_publish_peer(type, &payload);
}

/* ── State transitions ───────────────────────────────────────────────── */

static void transition_to_near_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	peer->state = KERFUR_PEER_STATE_NEAR;
	peer->near_since_ms = now_ms;
	peer->low_rssi_since_ms = 0;
	LOG_INF("peer 0x%08x -> NEAR (rssi_avg=%d, %s)", peer->ephemeral_id,
		rssi_from_avg(peer),
		peer->is_friend ? "friend" : "unknown");
	publish_peer_event(APP_EVENT_PEER_NEAR, peer, 0);
}

static void transition_to_interacting_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	/* If wall clock became available after initial detection, try a late
	 * friend resolution so encounters starting after sync are recognized. */
	if (!peer->is_friend) {
		int8_t fidx = resolve_friend_id(peer->ephemeral_id, now_ms);

		if (fidx >= 0) {
			peer->is_friend = true;
			peer->friend_index = fidx;
			LOG_INF("Late friend recognition for peer 0x%08x (slot %d)",
				peer->ephemeral_id, (int)fidx);
		}
	}

	peer->state = KERFUR_PEER_STATE_INTERACTING;
	peer->encounter_id = encounter_log_begin(peer->ephemeral_id, peer->character_id,
						 peer->friend_index, now_ms);
	peer->logged = true;
	publish_peer_event(APP_EVENT_ENCOUNTER_START, peer, 0);

	LOG_INF("Encounter start: peer 0x%08x %s", peer->ephemeral_id,
		peer->is_friend ? "(friend)" : "(unknown)");
}

static void transition_to_cooldown_locked(struct kerfur_peer *peer, int64_t now_ms)
{
	int32_t duration_s = 0;
	bool was_encounter = (peer->encounter_id != 0U);

	if (was_encounter) {
		duration_s = (int32_t)((now_ms - peer->first_seen_ms) / 1000);
		encounter_log_end(peer->encounter_id, now_ms);

		/* Increment before publishing so ENCOUNTER_END payload carries the
		 * total completed encounters including this one.  The behavior engine
		 * uses session_encounters ≥ 1 to recognise a "familiar" peer. */
		if (peer->session_encounters < UINT8_MAX) {
			peer->session_encounters++;
		}

		publish_peer_event(APP_EVENT_ENCOUNTER_END, peer, duration_s);

		/* Schedule a deferred friend encounter-count update so we do NOT
		 * call settings_save_one() while holding g_peer_mutex. */
		if (peer->friend_index >= 0) {
			g_pending_friend_count_mask |= ((uint64_t)1U << (uint8_t)peer->friend_index);
			g_pending_friend_count_ms = now_ms;
		}
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

/* ── Ingest ──────────────────────────────────────────────────────────── */

void kerfur_nearby_ingest_candidate(const struct kerfur_scan_candidate *cand)
{
	struct kerfur_peer *peer;
	struct kerfur_pet_snapshot snap;
	int8_t friend_idx;

	if ((cand == NULL) || (cand->ephemeral_id == 0U)) {
		return;
	}

	/* Reject own beacon echo (multi-path, test injections). */
	if (cand->ephemeral_id == g_current_ephemeral_id) {
		return;
	}

	kerfur_nearby_get_snapshot(&snap);

	/* Resolve friend status before acquiring peer_mutex.
	 * Lock order: secret → friend → peer (never reverse). */
	friend_idx = resolve_friend_id(cand->ephemeral_id, cand->timestamp_ms);

	k_mutex_lock(&g_peer_mutex, K_FOREVER);

	/* For confirmed friends: check whether this is a mid-encounter ephemeral
	 * ID rotation.  If so, update the existing peer entry silently so the
	 * encounter and behavior-engine context are not interrupted. */
	peer = peer_find_locked(cand->ephemeral_id);
	if ((peer == NULL) && (friend_idx >= 0)) {
		struct kerfur_peer *existing = peer_find_friend_locked(friend_idx);

		if (existing != NULL) {
			LOG_INF("Friend %d rotated id 0x%08x -> 0x%08x (state=%d)",
				(int)friend_idx, existing->ephemeral_id,
				cand->ephemeral_id, (int)existing->state);
			existing->ephemeral_id = cand->ephemeral_id;
			peer = existing;
		}
	}

	if (peer == NULL) {
		peer = peer_find_or_alloc_locked(cand->ephemeral_id, cand->timestamp_ms);
	}

	if (peer == NULL) {
		k_mutex_unlock(&g_peer_mutex);
		return;
	}

	/* Update friend identity on every beacon (handles late clock sync). */
	peer->friend_index = friend_idx;
	peer->is_friend = (friend_idx >= 0);

	if (peer->state == KERFUR_PEER_STATE_COOLDOWN) {
		/* Absorb packets but do not act until cooldown expires. */
		peer->last_seen_ms = cand->timestamp_ms;
		k_mutex_unlock(&g_peer_mutex);
		return;
	}

	peer_apply_candidate_locked(peer, cand);

	if (peer->state == KERFUR_PEER_STATE_NONE) {
		peer->state = KERFUR_PEER_STATE_SEEN;
		LOG_INF("peer 0x%08x first SEEN (rssi=%d, %s)",
			peer->ephemeral_id, cand->rssi,
			peer->is_friend ? "friend" : "unknown");
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

/* ── Tick (time-driven transitions) ─────────────────────────────────── */

/* Caps simultaneous NEAR/INTERACTING peers at MAX_ACTIVE_ENCOUNTERS by evicting
 * the weakest (lowest RSSI) entries.  This is purely a resource-protection
 * mechanism — it is NOT the social-overload signal.  Overload is driven by the
 * emotion engine's social_load accumulating over time. */
static void prune_active_encounters_locked(int64_t now_ms)
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

	if (count <= (size_t)MAX_ACTIVE_ENCOUNTERS) {
		return;
	}

	/* Sort by rssi_avg descending — weakest peers get pruned. */
	for (i = 0U; i < count; i++) {
		for (j = i + 1U; j < count; j++) {
			if (active[j]->rssi_avg_x16 > active[i]->rssi_avg_x16) {
				struct kerfur_peer *tmp = active[i];

				active[i] = active[j];
				active[j] = tmp;
			}
		}
	}

	for (i = (size_t)MAX_ACTIVE_ENCOUNTERS; i < count; i++) {
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

	prune_active_encounters_locked(now_ms);
	peer_evict_locked(now_ms);

	k_mutex_unlock(&g_peer_mutex);

	/* Flush deferred friend encounter-count updates outside peer_mutex.
	 * This avoids holding g_peer_mutex during NVS writes and keeps the
	 * lock order: secret → friend → peer never reversed. */
	if (g_pending_friend_count_mask != 0ULL) {
		uint64_t mask = g_pending_friend_count_mask;
		int64_t count_ms = g_pending_friend_count_ms;

		g_pending_friend_count_mask = 0ULL;

		for (i = 0U; i < (size_t)MAX_FRIENDS; i++) {
			if ((mask & ((uint64_t)1U << i)) == 0ULL) {
				continue;
			}
			k_mutex_lock(&g_friend_mutex, K_FOREVER);
			if (g_friends[i].in_use) {
				g_friends[i].encounter_count++;
				g_friends[i].last_seen_uptime_ms = count_ms;
			}
			k_mutex_unlock(&g_friend_mutex);
			save_friend((uint8_t)i);
			LOG_INF("Friend %u encounter count -> %u",
				(unsigned)i, g_friends[i].encounter_count);
		}
	}
}

/* ── Active peer count / dump ────────────────────────────────────────── */

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
