/*
 * Off-target golden-master test for the Kerfur nearby peer state machine.
 *
 * Compiles the REAL src/nearby/kerfur_nearby.c + encounter_log.c against the
 * tiny Zephyr shims in shim/, captures the app events the module publishes,
 * and drives synthetic scan candidates + ticks to assert the documented
 * transitions: SEEN -> NEAR -> INTERACTING -> COOLDOWN, the SEEN/CHECKING
 * split below the NEAR RSSI threshold, RSSI/idle hysteresis on the way out,
 * own-beacon echo rejection, and the MAX_ACTIVE_ENCOUNTERS prune.
 *
 * It is a GOLDEN MASTER: it pins today's behavior so the upcoming social
 * (greet/play handshake, encounter typing) changes can't silently regress the
 * detection layer. Build + run: bash tests/nearby_host/run.sh
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>   /* shim: __packed, k_*, kerfur_host_now_ms */
#include <zephyr/sys/util.h>

#include "core/app_event.h"
#include "nearby/encounter_log.h"
#include "nearby/kerfur_nearby.h"

/* Host monotonic clock referenced by the kernel shim's k_uptime_get(). */
int64_t kerfur_host_now_ms;

/* ── Captured event sink (replaces the real event bus) ──────────────────── */

struct captured {
	enum app_event_type type;
	struct app_event_peer peer;
};

static struct captured g_cap[256];
static int g_cap_n;

int app_event_publish_peer(enum app_event_type type, const struct app_event_peer *peer)
{
	if (g_cap_n < (int)ARRAY_SIZE(g_cap)) {
		g_cap[g_cap_n].type = type;
		g_cap[g_cap_n].peer = *peer;
		g_cap_n++;
	}
	return 0;
}

int app_event_publish_peer_with_timestamp(enum app_event_type type,
					  const struct app_event_peer *peer,
					  int64_t timestamp_ms)
{
	(void)timestamp_ms;
	return app_event_publish_peer(type, peer);
}

static void cap_reset(void)
{
	g_cap_n = 0;
}

static int cap_count(enum app_event_type t)
{
	int n = 0;

	for (int i = 0; i < g_cap_n; i++) {
		if (g_cap[i].type == t) {
			n++;
		}
	}
	return n;
}

/* Index of the first captured event of type t, or -1. */
static int cap_first(enum app_event_type t)
{
	for (int i = 0; i < g_cap_n; i++) {
		if (g_cap[i].type == t) {
			return i;
		}
	}
	return -1;
}

/* True if every type in seq[] appears in capture order (a subsequence). */
static bool cap_in_order(const enum app_event_type *seq, int n)
{
	int i = 0;

	for (int c = 0; (c < g_cap_n) && (i < n); c++) {
		if (g_cap[c].type == seq[i]) {
			i++;
		}
	}
	return i == n;
}

/* ── Assertion harness ──────────────────────────────────────────────────── */

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		g_checks++;                                                     \
		if (cond) {                                                     \
			printf("[ ok ] ");                                     \
		} else {                                                       \
			printf("[FAIL] ");                                     \
			g_failures++;                                          \
		}                                                              \
		printf(__VA_ARGS__);                                           \
		printf("\n");                                                  \
	} while (0)

/* ── Candidate helpers ──────────────────────────────────────────────────── */

static uint8_t g_seq;

static void feed_full(uint32_t id, int8_t rssi, uint8_t mode_expr,
		      uint8_t status_flags, uint8_t social_event, int64_t t)
{
	struct kerfur_scan_candidate c;

	memset(&c, 0, sizeof(c));
	c.ephemeral_id = id;
	c.rssi = rssi;
	c.mode_expr = mode_expr;
	c.status_flags = status_flags;
	c.social_event = social_event;
	c.sequence = g_seq++;
	c.timestamp_ms = t;
	kerfur_nearby_ingest_candidate(&c);
}

static void feed(uint32_t id, int8_t rssi, int64_t t)
{
	feed_full(id, rssi, 0, 0, 0, t);
}

static uint32_t own_beacon_id(void)
{
	uint8_t buf[64];
	size_t n = kerfur_nearby_build_beacon(buf, sizeof(buf));
	struct kerfur_scan_candidate own;

	if ((n == 0) || !kerfur_nearby_parse_beacon(buf, n, &own)) {
		return 0;
	}
	return own.ephemeral_id;
}

/* Drive one peer all the way to INTERACTING; returns the time after the
 * encounter has started. */
static int64_t bring_to_interacting(uint32_t id, int64_t t0)
{
	feed(id, -68, t0);            /* NONE -> SEEN */
	feed(id, -68, t0 + 1000);    /* SEEN -> NEAR (seen_count >= 2, in window) */
	feed(id, -68, t0 + 4200);    /* NEAR held >= 3000 ms -> INTERACTING */
	return t0 + 4200;
}

/* ── Scenarios ──────────────────────────────────────────────────────────── */

static void test_full_lifecycle(void)
{
	printf("\n-- lifecycle: SEEN -> NEAR -> INTERACTING -> COOLDOWN --\n");
	cap_reset();
	g_seq = 0;

	int64_t t = 100000;
	int64_t enc = bring_to_interacting(0xA1A1A1A1u, t);

	const enum app_event_type up[] = {
		APP_EVENT_PEER_SEEN, APP_EVENT_PEER_NEAR, APP_EVENT_ENCOUNTER_START,
	};
	CHECK(cap_in_order(up, 3), "rise order SEEN, NEAR, ENCOUNTER_START");
	CHECK(cap_count(APP_EVENT_PEER_SEEN) == 1, "exactly one PEER_SEEN");
	CHECK(cap_count(APP_EVENT_PEER_NEAR) == 1, "exactly one PEER_NEAR");
	CHECK(cap_count(APP_EVENT_ENCOUNTER_START) == 1, "exactly one ENCOUNTER_START");

	/* Stranger, no clock: not a friend, friend_index -1, first meeting. */
	int idx = cap_first(APP_EVENT_ENCOUNTER_START);
	CHECK((idx >= 0) && !g_cap[idx].peer.is_friend, "encounter peer is not a friend");
	CHECK((idx >= 0) && (g_cap[idx].peer.friend_index == -1), "friend_index is -1");
	CHECK((idx >= 0) && (g_cap[idx].peer.session_encounters == 0),
	      "session_encounters 0 on first meeting");

	/* Go quiet: idle past ENCOUNTER_IDLE_MS (15 s) -> cooldown. */
	kerfur_nearby_tick(enc + 16000);

	CHECK(cap_count(APP_EVENT_ENCOUNTER_END) == 1, "one ENCOUNTER_END after idle");
	CHECK(cap_count(APP_EVENT_PEER_LOST) >= 1, "PEER_LOST fired on cooldown");

	int eidx = cap_first(APP_EVENT_ENCOUNTER_END);
	CHECK((eidx >= 0) && (g_cap[eidx].peer.session_encounters == 1),
	      "ENCOUNTER_END reports session_encounters == 1");
	CHECK((eidx >= 0) && (g_cap[eidx].peer.duration_s > 0),
	      "ENCOUNTER_END carries a positive duration");
}

static void test_checking_below_near_threshold(void)
{
	printf("\n-- weak signal: SEEN -> CHECKING (never NEAR) --\n");
	/* The module has no public reset; scenarios share state but use distinct
	 * peer ids + time bases, and cap_reset() isolates the event assertions. */
	cap_reset();
	g_seq = 0;

	int64_t t = 200000;

	feed(0xB2B2B2B2u, -85, t);          /* below NEAR (-75): just SEEN */
	feed(0xB2B2B2B2u, -85, t + 800);    /* seen_count >= 2 but weak -> CHECKING */

	CHECK(cap_count(APP_EVENT_PEER_SEEN) == 1, "one PEER_SEEN");
	CHECK(cap_count(APP_EVENT_PEER_CHECKING) >= 1, "PEER_CHECKING for a weak peer");
	CHECK(cap_count(APP_EVENT_PEER_NEAR) == 0, "no PEER_NEAR below threshold");
	CHECK(cap_count(APP_EVENT_ENCOUNTER_START) == 0, "no encounter below threshold");
}

static void test_near_idle_timeout(void)
{
	printf("\n-- NEAR idle timeout -> PEER_LOST (no encounter) --\n");
	cap_reset();
	g_seq = 0;

	int64_t t = 300000;

	feed(0xC3C3C3C3u, -68, t);
	feed(0xC3C3C3C3u, -68, t + 1000);   /* -> NEAR */
	CHECK(cap_count(APP_EVENT_PEER_NEAR) == 1, "reached NEAR");

	/* Never reached INTERACTING; idle past PEER_LOST_IDLE_MS (12 s). */
	kerfur_nearby_tick(t + 1000 + 13000);

	CHECK(cap_count(APP_EVENT_PEER_LOST) >= 1, "PEER_LOST on NEAR idle");
	CHECK(cap_count(APP_EVENT_ENCOUNTER_END) == 0, "no ENCOUNTER_END (never interacted)");
}

static void test_own_echo_rejected(void)
{
	printf("\n-- own-beacon echo is ignored --\n");
	cap_reset();
	g_seq = 0;

	uint32_t own = own_beacon_id();

	CHECK(own != 0, "own beacon id resolved");
	feed(own, -50, 400000);
	feed(own, -50, 400500);
	CHECK(g_cap_n == 0, "no events published for our own ephemeral id");
}

static void test_multi_peer_prune(void)
{
	printf("\n-- MAX_ACTIVE_ENCOUNTERS prune (cap NEAR/INTERACTING) --\n");
	cap_reset();
	g_seq = 0;

	int64_t t = 500000;

	/* Flush peers left active by earlier scenarios so the cap assertion below
	 * counts only this scenario's peers (no public reset; shared module
	 * state). By t=500000 every prior peer is well past its idle windows. */
	kerfur_nearby_tick(t - 1);
	CHECK(kerfur_nearby_active_peer_count() == 0, "no active peers leak in from prior tests");

	/* Push 6 distinct peers to NEAR (cap is 5). Distinct RSSI so the prune
	 * target (weakest) is deterministic. */
	for (int i = 0; i < 6; i++) {
		uint32_t id = 0xD0D00000u + (uint32_t)i;
		int8_t rssi = (int8_t)(-60 - i * 3);   /* -60 .. -75, all >= NEAR */

		feed(id, rssi, t);
		feed(id, rssi, t + 500);
	}
	CHECK(cap_count(APP_EVENT_PEER_NEAR) == 6, "all 6 reached NEAR");

	kerfur_nearby_tick(t + 600);   /* prune runs here */

	CHECK(kerfur_nearby_active_peer_count() == 5,
	      "active peers capped at MAX_ACTIVE_ENCOUNTERS (5)");
}

/* Find the logged encounter for a peer id and return its type, or -1. */
static int encounter_type_of(uint32_t id)
{
	struct encounter_record recs[16];
	size_t n = encounter_log_dump(recs, ARRAY_SIZE(recs));

	for (size_t i = 0; i < n; i++) {
		if (recs[i].ephemeral_id == id) {
			return (int)recs[i].encounter_type;
		}
	}
	return -1;
}

static void test_walk_together_typing(void)
{
	printf("\n-- encounter typing: WALK_TOGETHER when both walking --\n");
	cap_reset();
	g_seq = 0;

	/* Tell the nearby module THIS Kerfur is walking (publishes the snapshot
	 * the ingest path reads). */
	struct pet_state ps;

	memset(&ps, 0, sizeof(ps));
	ps.walking_active = true;
	ps.current_mode = PET_MODE_WALK_AWAKE;
	kerfur_nearby_on_pet_tick(&ps);

	int64_t t = 600000;
	uint32_t walker = 0xE5E5E5E5u;

	/* Peer also advertises walking, carried through to INTERACTING. */
	feed_full(walker, -66, 0, KFR_STATUS_WALKING, 0, t);
	feed_full(walker, -66, 0, KFR_STATUS_WALKING, 0, t + 1000);
	feed_full(walker, -66, 0, KFR_STATUS_WALKING, 0, t + 4200);
	CHECK(cap_count(APP_EVENT_ENCOUNTER_START) == 1, "encounter started while both walking");
	CHECK(encounter_type_of(walker) == KERFUR_ENCOUNTER_WALK_TOGETHER,
	      "encounter typed WALK_TOGETHER");

	/* Control: we are still walking, but a peer that does NOT advertise
	 * walking must stay the default FIRST_CONTACT. */
	cap_reset();
	uint32_t stander = 0xE6E6E6E6u;

	feed_full(stander, -66, 0, 0, 0, t + 8000);
	feed_full(stander, -66, 0, 0, 0, t + 9000);
	feed_full(stander, -66, 0, 0, 0, t + 12200);
	CHECK(encounter_type_of(stander) == KERFUR_ENCOUNTER_FIRST_CONTACT,
	      "non-walking peer stays FIRST_CONTACT");

	/* Leave the snapshot non-walking so it can't surprise later tests. */
	memset(&ps, 0, sizeof(ps));
	kerfur_nearby_on_pet_tick(&ps);
}

/* Social event currently carried by our own broadcast beacon. */
static uint8_t beacon_social(void)
{
	uint8_t buf[64];
	size_t n = kerfur_nearby_build_beacon(buf, sizeof(buf));
	struct kerfur_scan_candidate c;

	if ((n == 0) || !kerfur_nearby_parse_beacon(buf, n, &c)) {
		return 0xFFu;
	}
	return c.social_event;
}

static void test_tx_social_emit(void)
{
	printf("\n-- TX: emitted social event rides the beacon for its window --\n");

	/* build_beacon reads the host clock for the TTL check; drive it directly. */
	kerfur_host_now_ms = 700000;
	kerfur_nearby_emit_social(KFR_SOCIAL_NONE);
	CHECK(beacon_social() == KFR_SOCIAL_NONE, "beacon defaults to social NONE");

	kerfur_nearby_emit_social(KFR_SOCIAL_GREET);
	CHECK(beacon_social() == KFR_SOCIAL_GREET, "beacon carries GREET after emit");

	kerfur_host_now_ms += 3000;   /* still inside KFR_SOCIAL_TX_TTL_MS (4000) */
	CHECK(beacon_social() == KFR_SOCIAL_GREET, "GREET still broadcast inside the window");

	kerfur_host_now_ms += 2000;   /* total 5000 > TTL -> expired */
	CHECK(beacon_social() == KFR_SOCIAL_NONE, "GREET expires after its window");

	kerfur_nearby_emit_social(KFR_SOCIAL_PLAY_INVITE);
	CHECK(beacon_social() == KFR_SOCIAL_PLAY_INVITE, "beacon carries PLAY_INVITE");
	kerfur_nearby_emit_social(KFR_SOCIAL_NONE);
	CHECK(beacon_social() == KFR_SOCIAL_NONE, "emit NONE clears immediately");
}

static void test_rx_social_translation(void)
{
	printf("\n-- RX: peer social event -> engine event (rising edge, de-duped) --\n");
	cap_reset();
	g_seq = 0;

	int64_t t = 800000;
	uint32_t id = 0xF7F7F7F7u;

	/* First GREET from a peer -> exactly one PEER_GREET. */
	feed_full(id, -65, 0, 0, KFR_SOCIAL_GREET, t);
	CHECK(cap_count(APP_EVENT_PEER_GREET) == 1, "first GREET -> one PEER_GREET");

	/* Repeated GREET (sender's window still open) -> no re-fire. */
	feed_full(id, -65, 0, 0, KFR_SOCIAL_GREET, t + 500);
	CHECK(cap_count(APP_EVENT_PEER_GREET) == 1, "repeated GREET does not re-fire");

	/* Sender drops to NONE, then greets again -> a fresh rising edge. */
	feed_full(id, -65, 0, 0, KFR_SOCIAL_NONE, t + 1000);
	feed_full(id, -65, 0, 0, KFR_SOCIAL_GREET, t + 1500);
	CHECK(cap_count(APP_EVENT_PEER_GREET) == 2, "GREET after NONE re-fires");

	/* A peer play-invite maps onto the existing engine play event. */
	feed_full(id, -65, 0, 0, KFR_SOCIAL_PLAY_INVITE, t + 2000);
	CHECK(cap_count(APP_EVENT_PEER_PLAY_INVITE) == 1, "PLAY_INVITE -> one PEER_PLAY_INVITE");
}

static void test_beacon_refresh_signal(void)
{
	printf("\n-- TX refresh signal: emit + window expiry both request a rebuild --\n");

	kerfur_host_now_ms = 900000;
	/* Drain any leftover pending state from the TX test. */
	kerfur_nearby_emit_social(KFR_SOCIAL_NONE);
	(void)kerfur_nearby_take_beacon_refresh();

	kerfur_nearby_emit_social(KFR_SOCIAL_GREET);
	CHECK(kerfur_nearby_take_beacon_refresh() == true, "emit requests a beacon refresh");
	CHECK(kerfur_nearby_take_beacon_refresh() == false, "refresh request is one-shot");
	CHECK(beacon_social() == KFR_SOCIAL_GREET, "still broadcasting GREET inside window");

	/* Past the window: the next poll clears it and asks for one more rebuild. */
	kerfur_host_now_ms += 5000;
	CHECK(kerfur_nearby_take_beacon_refresh() == true, "window expiry requests a clear refresh");
	CHECK(beacon_social() == KFR_SOCIAL_NONE, "beacon cleared after expiry");
	CHECK(kerfur_nearby_take_beacon_refresh() == false, "no further refresh once cleared");
}

int main(void)
{
	printf("=== Kerfur nearby host test ===\n");

	kerfur_nearby_init();

	test_full_lifecycle();
	test_checking_below_near_threshold();
	test_near_idle_timeout();
	test_own_echo_rejected();
	test_multi_peer_prune();
	test_walk_together_typing();
	test_tx_social_emit();
	test_rx_social_translation();
	test_beacon_refresh_signal();

	printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
	if (g_failures != 0) {
		printf("FAILED\n");
		return 1;
	}
	printf("all nearby state-machine checks pass\n");
	return 0;
}
