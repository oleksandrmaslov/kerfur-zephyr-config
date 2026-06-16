# Kerfus / Kerfur — Next Steps Roadmap

> Created: 2026-06-09 (companion to `KERFUS_PROJECT_STATE.md`)
> Status legend: TODO · IN PROGRESS · DONE · BLOCKED
> Priority legend: P0 (do first) · P1 · P2 · P3 (later/vision)

This is a practical roadmap, ordered to stabilize first, then reach a "living
prototype" MVP, then grow toward the social/app vision. Each task lists why,
affected files, priority, and status.

---

## Session progress (updated 2026-06-15/16 — nearby social depth + handshake)

Focus: deepen Kerfur↔Kerfur social behavior. Verified true state in code first —
the contagion/greeting/vibe layer is genuinely rich; the gaps below are real.

- **New off-target harness** `tests/nearby_host/` (`bash tests/nearby_host/run.sh`):
  compiles the real `kerfur_nearby.c` + `encounter_log.c` against Zephyr shims,
  captures published app events, and golden-masters the peer state machine
  (SEEN→NEAR→INTERACTING→COOLDOWN, RSSI near/lost + idle hysteresis, own-beacon
  echo reject, active-encounter prune). 26 checks, green. The `kerfur_nearby`
  half of M3.
- **Encounter typing (nearby side):** `encounter_log_set_type()` was defined but
  **never called** — every encounter logged as FIRST_CONTACT. Now typed
  `WALK_TOGETHER` when this Kerfur and the peer both report walking during it.
  Host-tested (with a non-walking control). GREETING/PLAY typing deferred to the
  handshake (Track 2).
- **Walking-together contagion (engine side):** `behavior_engine.c`
  ENCOUNTER_START adds warmth/attachment/mood (+2) when both are walking — the
  shared-walk beat. Reads the peer's broadcast mode (stays decoupled from the
  nearby driver). Traced, **not flashed** (owner builds). Corrects a doc/code
  drift in `KERFUS_EMOTION_RUNTIME.md` §2 (a shared-walk mood input that was
  documented but unimplemented).
- **Kerfur↔Kerfur greet handshake (Track 2 — built, owner chose rate-limited
  adv-restart refresh):** the social handshake was inert — `build_beacon()`
  hardcoded `KFR_SOCIAL_NONE` and a received `social_event` was never turned
  into an engine event. Now:
  - **TX:** `kerfur_nearby_emit_social()` sets a pending social event broadcast
    for a `KFR_SOCIAL_TX_TTL_MS` (4 s) window; `build_beacon()` emits it.
    `kerfur_nearby_take_beacon_refresh()` signals when the payload must be
    rebuilt (fresh emit or expired window). **Host-tested.**
  - **RX:** ingest detects a social-event *rising edge* (de-duped per peer) and
    publishes `PEER_GREET` / `PEER_GREET_ACK` / `PEER_PLAY_INVITE` / `_ACK`.
    **Host-tested.** New `APP_EVENT_PEER_GREET[_ACK]` events.
  - **Orchestration (`app.c`):** greet on `ENCOUNTER_START`, ack on `PEER_GREET`,
    then poll the refresh signal → `ble_manager_request_beacon_refresh()`.
  - **Refresh (`ble_manager.c`):** rate-limited (`KERFUR_BEACON_REFRESH_MIN_MS`
    1.5 s) reuse of the proven `g_adv_restart_work` path; no-ops while connected.
    **Touches the adv path — needs two-device verification + a flash.**
  - **Engine:** reacts to received greet/greet-ack (warmth + bow, bounce if
    playful). Traced.
  - **Single-device demo:** `kerfur nearby inject greet <id>`.
  - **Still TODO:** GREETING/PLAY encounter-type classification on a completed
    handshake; engine-initiated play invites (escalation); two-device on-air
    verification of the refresh churn.

## Session progress (updated 2026-06-15 — BLE connect/pairing rework)

Focus: fix the phone-connect bug (BT icon then freeze, no pairing prompt, ANCS
only after several resets). Root-caused from the code; **not on-device-verified**
(owner builds + flashes manually). All in `src/ble/ble_manager.c` + `prj.conf`.

- **Pairing now "Just Works":** the auth callbacks registered `passkey_display`
  + `passkey_confirm`, which gave the device IO capability **DisplayYesNo** and
  let iOS pick a numeric/MITM flow a no-display device can't complete — so the
  "Pair" prompt often never appeared. Dropped those callbacks (kept
  `pairing_confirm`/`cancel`) → IO cap **NoInputNoOutput** → iOS shows the simple
  "Bluetooth Pairing Request → Pair" dialog and auto-pairs (no MITM).
- **Stale-bond self-heal:** `security_changed_cb` now `bt_unpair()`s + disconnects
  on `BT_SECURITY_ERR_PIN_OR_KEY_MISSING` (the definitive key-mismatch signal),
  so a dead bond from a UF2 reflash / phone-forget recovers on its own instead of
  needing the "reset a few times" dance. (Two-sided: the user may still need to
  forget the device on the phone once.)
- **Directed advertising dropped:** every disconnect used to switch to low-duty
  *directed* advertising at the single stored bond, which silently blocked a
  *fresh* phone (and stalled completely on a stale bond). Now always
  connectable-undirected (fast→slow). Removed `g_prefer_directed_reconnect`, the
  directed adv param/phase, and the bond-snapshot helper.
- **UF2 reflash hardening:** `prj.conf` now sets `CONFIG_BT_GATT_SERVICE_CHANGED`
  + `CONFIG_BT_GATT_CACHING` explicitly so a bonded phone re-discovers the GATT
  DB after the app image changes (handles point at the right ANCS attributes).
- **Field bond reset:** shell `ble unpair_all` (existing) is the documented path;
  no hardware gesture yet (deferred by owner choice).
- **Also this session (engine):** broadened F1 expression-routing gradations —
  added the `OVERSTIMULATED↔DRAINED` burnout-crash edge and locked 7 named
  companion arcs in `tools/appraisal_calibrate.py` (all green).
- **Verify on flash:** capture USB-CDC serial during a connect; expect the iOS
  Pair prompt, `BLE pairing complete (bonded=yes)`, then ANCS discovery — no
  resets. `ble status` / `ble unpair_all` available in the shell.

## Session progress (updated 2026-06-15 — face fixes + render test harness)

Focus: fix the faces and perfect the face/engine runtime (owner has not flashed
yet; all changes traced + host-tested, not on-device-verified).

- **Face bug fixed:** OVERSTIMULATED never showed its spiral pupils — a
  `swap-on-blink` pupil swap on a recipe with blinking disabled deadlocked
  forever. `resolve_pupil_swap()` now falls back to a time-based settle when
  the blink profile is disabled. General invariant: a pupil swap always
  resolves.
- **Idle motion reworked (owner request):** removed the constant cyclic
  up-right-down-left ambient pupil drift (`resolve_ambient_pupil_drift` deleted
  + plumbing simplified); kept the occasional slow wander glance. Breathing bob
  made much subtler (brief ~18 % lift over a 5.5–9 s cycle vs the old 50 % of
  2.8–5.2 s). Reaction whisker-wiggles untouched; blinking untouched.
- **Render test harness added:** `tests/face_host/` (host gcc, no Zephyr) drives
  all 13 expressions × transitions × reactions through the real
  `face_runtime.c` + generated tables — ~1.03M checks, all green; catches the
  pupil-swap deadlock if reintroduced. Engine selection still green via
  `tools/appraisal_calibrate.py`.
- **Audit result:** OVERSTIMULATED was the only structural render defect; the
  other 12 faces and all reactions pass the harness.
- **Next (needs owner flash):** per-expression visual polish (layout/look
  targets/whisker edge framing), then deeper engine/behavior tuning.

## Session progress (updated 2026-06-10)

**Done this session (compiles clean per owner; OOM during one build was just a
parallel-jobs RAM ceiling, not a code issue):**
- IMU/behavior bug fixes: `g_mc` mutex (race), classifier no longer eats its own
  events, battery-critical strand recovery, night-detection guard.
- Battery/charger scaffold: `battery_monitor.c` (stub backend = zero behavior
  change) + real `bq25630.c` (BQ25630 charger + MAX17048 gauge) gated by
  `CONFIG_KERFUR_CHARGER_BQ2563X`. Builds verified (FLASH ~72%).
- **Touch system rebuilt** (was a single-GPIO stub):
  - `touch_gestures.c` — hardware-agnostic gesture engine (tap/double-tap/
    long-press/hold/stroke/repeated-stroke), pure + testable.
  - `touch_input.c` — GPIO front-end now feeds the gesture engine.
  - `iqs7222a.c` — full Azoteq IQS7222A driver: I2C + active-low RDY, datasheet
    bring-up (verify 840 → SW-reset → push config → ACK reset → re-ATI → wait
    ATI → event mode), reads 0x10..0x17 (touch bitmask + reset detect), reset
    recovery, dedicated workqueue for blocking steps. Gated by
    `CONFIG_KERFUR_TOUCH_IQS7222A`; feeds the same gesture engine.

### Touch system — remaining (hardware-gated) — IN PROGRESS
- **Config export (BLOCKED on flex tuning):** replace `kerfur_iqs7222a_config.c`
  with the Azoteq IQS7222A Configuration Tool export for the petting flex.
  Until then the chip runs power-up defaults (untuned).
- **Board overlay (BLOCKED on pin map; schematic is unwired):** add a
  `touch_iqs0` node so the driver binds. Template:
  ```dts
  &i2c0 {
      iqs7222a: iqs7222a@44 {
          compatible = "i2c-device";   /* app-level driver; no DT binding needed */
          reg = <0x44>;
          rdy-gpios = <&gpio0 PIN (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; /* TODO: real RDY pin */
      };
  };
  / { aliases { touch-iqs0 = &iqs7222a; }; };
  ```
  Then build with `-DCONFIG_KERFUR_TOUCH_IQS7222A=y`.
- **Nice-to-have:** dedicated `APP_EVENT_USER_DOUBLE_TAP` (currently mapped to
  TAP); stroke *direction* from channel ordering once the zone layout is known;
  a `kerfur touch ...` shell debug command; ztest for the gesture engine.

---

## Immediate tasks (stabilize)

### I1. Decide canonical name (Kerfus vs Kerfur)
- **Why:** Vision docs (`01/02/Master_goal`) and these audit docs say **Kerfus**;
  all code, the BLE device name, and `CONFIG_KERFUR_*` say **Kerfur**. Pick one
  brand so future work and the companion app stay consistent. (The product docs
  themselves are complete and consistent — no longer a gap.)
- **Affected:** decision only; if "Kerfus" wins, a later mechanical rename of code
  symbols/BLE name/Kconfig (large, do deliberately, not now).
- **Priority:** P0 (decision) / P3 (any rename)
- **Status:** TODO (owner decision needed — see KERFUS_AGENT_HANDOFF "open questions")

### I2. Pin and document one Zephyr SDK version
- **Why:** `build_codex` cache pins SDK 0.16.5 (path now broken); machine has
  1.0.1. Incremental builds fail with a misleading toolchain error.
- **Affected:** README build section; delete/regenerate stale `build_codex/` and
  `build/nearby_test/`.
- **Priority:** P0
- **Status:** TODO

### I3. Confirm a clean pristine build in CI-like conditions
- **Why:** Prove the tree compiles on the current toolchain and keep it that way.
- **Affected:** build only (no source).
- **Priority:** P0
- **Status:** DONE — pristine build verified 2026-06-09 (SDK 1.0.1, `nice_nano`,
  `-DSHIELD=kerfur_oled`): exit 0, FLASH 72.3% / RAM 31.5%. See
  `KERFUS_PROJECT_STATE.md` §6. (Keep CI green; recheck headroom as features grow.)

### I4. Reconcile drive-variable vocabulary (doc ↔ code)
- **Why:** `CLAUDE.md` §7 names mood/affection/social_interest/alertness/tiredness;
  code uses energy/sleepiness/attachment/boredom/stress/arousal/social_load/trust/
  curiosity. Pick one canonical vocabulary and map it.
- **Affected:** `02_…` doc, comments in `include/behavior/pet_state.h`.
- **Priority:** P1
- **Status:** PARTIALLY DONE (2026-06-11) — a real `mood` drive now exists in
  code, and the doc↔code mapping table lives in
  `KERFUS_EMOTION_RUNTIME.md` §1. Remaining: decide whether to mechanically
  rename the other drives.

---

## MVP tasks (first living Kerfus prototype)

The MVP per `CLAUDE.md` §19 is *almost* met. The missing pieces are battery
realism, true sleep, and confidence (tests). Most face/touch/motion/notification
MVP items already work.

### M1. Real battery + charger sensing driver
- **Why:** P0 product gap. `power_manager.c` never reads hardware; every layer
  already reacts to `BATTERY_LOW/CRITICAL/PERCENT_UPDATE` + `CHARGER_*` events
  that only mocks emit today. 18650 cell per spec.
- **How:** new `src/power/battery_monitor.c` behind the existing events —
  ADC/fuel-gauge read + charge-detect GPIO → publish battery % and charger
  events on the bus. Keep it a driver; do not put emotional logic in it.
- **Affected:** new `battery_monitor.c/.h`, `CMakeLists.txt`, `prj.conf` (ADC),
  board overlays (ADC channel / charge-stat pin), `Kconfig`.
- **Priority:** P0
- **Status:** TODO (BLOCKED on hardware pin assignment — see open questions)

### M2. True low-power sleep on SLEEP_REQUEST
- **Why:** `SLEEP_REQUEST` is emitted/acked but the device never actually sleeps;
  18650 runtime depends on it.
- **How:** on `APP_EVENT_SLEEP_REQUEST` drive display off, lower BLE adv interval,
  drop IMU to wake-on-motion, enter Zephyr PM; configure motion/touch/BLE wake.
- **Affected:** `power_manager.c`, `display_policy.c`, `motion_sensor.c`,
  `ble_manager.c`, `prj.conf` (CONFIG_PM).
- **Priority:** P1
- **Status:** TODO

### M3. Unit tests for behavior + nearby state machines
- **Why:** No safety net; these are the product's brain and are pure enough to
  test off-target (native_sim / ztest).
- **How:** feed synthetic event sequences, assert mode/expression transitions,
  cooldowns, peer state transitions, RSSI hysteresis.
- **Affected:** new `tests/` tree with `testcase.yaml`; possibly small seams in
  `behavior_engine`/`kerfur_nearby` to allow injecting `now_ms`.
- **Priority:** P1
- **Status:** IN PROGRESS (2026-06-15) — three off-target harnesses now exist
  and pass: `tools/appraisal_calibrate.py` (engine selection: 20 scenarios + 7
  stability checks), `tests/face_host/` (face rendering, ~2.25M checks, guards
  the pupil-swap deadlock), and `tests/nearby_host/` (the `kerfur_nearby`
  peer-state / RSSI-hysteresis / prune / encounter-typing machine, 26 checks).
  Remaining: `behavior_engine` event-sequence tests; optional ztest/twister
  integration so they run in CI alongside `west`.

### M4. Persistence / personality module
- **Why:** `CLAUDE.md` §14. Survive reboot: device name, personality, affection,
  known/friend peers, quiet settings. Personality must influence behavior, not be
  a label.
- **How:** `src/state/persistence.c` over Zephyr `settings`/NVS (already enabled);
  load at boot into `pet_state` seed + a `personality` profile that biases drive
  decay/weights in `behavior_engine`.
- **Affected:** new module, `behavior_engine.c`, `pet_state.h`.
- **Priority:** P2
- **Status:** LARGELY DONE (2026-06-11) — `src/behavior/emotion_memory.c`
  persists attachment/trust/mood/lifetime-pets/personality
  (`kerfur/emo/traits`), and 5 personality profiles genuinely bias gains in
  `behavior_engine.c` (see `KERFUS_EMOTION_RUNTIME.md` §3–4). Remaining: device
  name, known/friend peers (see L4), quiet-hours settings.

---

## Face / expression quality (2026-06-15, owner-reported)

### F1. Expression transition routing (smooth face election)
- **Why:** the election in `update_expression` (`behavior_engine.c`) jumps
  **directly** to the best-scoring expression and several overrides set it
  **instantly with no easing**, so emotionally-distant changes look rough:
  - waking from sleep can jump `ASLEEP → CALM` instead of easing
    `ASLEEP → SLEEPY → (COZY/CALM)` (the grogginess bias often loses to a
    pickup arousal spike, and nothing forces the first post-wake step to be
    SLEEPY);
  - petting an angry pet hits the `pet_recent` override which forces
    `ANGRY → HAPPY` instantly (bypasses all hysteresis), then snaps back when
    the pet window ends.
- **Root causes (with refs):** instant overrides
  ([behavior_engine.c:2455-2473](../src/behavior/behavior_engine.c#L2455-L2473))
  bypass hysteresis; the main election commits the best expression directly
  ([behavior_engine.c:2530-2542](../src/behavior/behavior_engine.c#L2530-L2542));
  `transition_affinity` only *biases* toward neighbours (≤5), it does not
  *constrain* the path.
- **How (clean rework):** add an emotional-adjacency transition policy — when
  the elected target is not adjacent to the current expression, step through a
  shared neighbour (usually CALM) instead of jumping; route the petting and
  wake overrides the same way (soothe `ANGRY → CALM → HAPPY`; always wake
  `ASLEEP → SLEEPY` first for the grogginess window). Keep destinations
  unchanged (appraisal suite stays green); add transition-path tests.
- **Affected:** `behavior_engine.c` (`update_expression`, overrides,
  `transition_affinity` graph), `tools/appraisal_calibrate.py` (path tests),
  possibly a behavior-engine host harness.
- **Priority:** P1 · **Status:** DONE (2026-06-15, owner chose *full adjacency
  routing*). Two-layer election: appraisal+hysteresis pick a stable target;
  `expr_first_hop` (BFS over the affinity graph, +`ASLEEP↔SLEEPY` edge) walks
  the face there one hop per `EXPR_ROUTE_HOP_MS` (600 ms). Petting eases
  (soothes ANGRY→CALM→…→HAPPY), waking eases (ASLEEP→SLEEPY→…), forced/asleep
  still snap. Validated by `appraisal_calibrate.py` routing checks (169 routes
  + named eases, green). Not on-device-verified yet (owner builds). Possible
  follow-up: scale `EXPR_ROUTE_HOP_MS` with arousal; situation-aware routing
  so worn-quiet transit avoids disallowed intermediates.

### F2. One-frame pupil glitch after a reaction blink
- **Why:** owner saw a single frame on blink-reopen with the left pupil missing
  and the right pupil shoved to the top-left — likely an asymmetric base
  expression (e.g. ANGRY's `{16,17}`/`{17,16}` pupil layout) caught mid-jump,
  and/or pupil offsets reading as zero for the reopen frame while a reaction is
  still active (offsets are only recomputed when `dynamic_allowed || idle_pupils`).
- **How:** extend `tests/face_host` with per-frame blink-boundary checks
  (pupil presence symmetry + offset continuity across reopen) to reproduce
  deterministically, then fix (align the `blink_active` threshold with the
  renderer open/close hysteresis, and hold the last pupil offset through the
  closed→open boundary). Likely reduced further by F1.
- **Affected:** `src/ui/face_runtime.c`, `tests/face_host/`.
- **Priority:** P1 · **Status:** DONE (2026-06-15). The host harness (now
  ~2.25M checks incl. blink-during-transition) showed the pupil *offset* path
  is clean — the bad frame was the eye-white **teleport** on an expression
  change (only brows/mouth/openness eased). Fixed by easing eye-white position
  via four transition channels (pupils ride along). Not on-device-verified yet.

## Later tasks (app integration, OTA, social, advanced)

### L1. Finish Android companion / Gadgetbridge payloads
- Richer notification parsing beyond compact categories; finish the
  notification-enable helper. **Affected:** `ble_manager.c`, `docs/gadgetbridge/`.
  Priority P2. Status TODO.

### L2. Companion app / mini app API surface
- Onboarding, settings, notification filters, quiet hours, important contacts,
  battery + firmware reporting, diagnostics, friend confirmation for nearby.
  **Affected:** `ble_manager.c` companion service, new app-API module.
  Priority P3. Status TODO.

### L3. OTA / DFU
- MCUboot is vendored; wire a DFU path + dual-slot partitioning for nice_nano.
  Priority P3. Status TODO.

### L4. Friend graph for nearby ("First Kerfus meet each other…")
- Persistent friend identity gated by explicit pairing/confirmation in the app
  (privacy rule: no hidden tracking; ephemeral IDs stay default).
  **Affected:** `kerfur_nearby.c`, persistence, companion API. Priority P3. TODO.

### L5. Move remaining hand-maintained face metadata into codegen
- Reduce drift between `assets/face/*.json` and hand-written glue.
  **Affected:** `tools/face_codegen.py`, `face_runtime.c`. Priority P3. TODO.

### L6. Output expansion (haptics / LED / sound)
- Add as new output modules behind the event bus when hardware is defined.
  Priority P3. TODO.

---

## Suggested execution order

1. I1, I2, I3 (stabilize + provable build) →
2. I4 (vocabulary) + M3 (tests, so later changes are safe) →
3. M1 (battery) + M2 (sleep) — the two real "living device" gaps →
4. M4 (persistence/personality) →
5. L-series as product/app matures.
