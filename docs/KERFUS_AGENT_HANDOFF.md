# Kerfus / Kerfur — Agent Handoff

> For the next Claude / Codex / ChatGPT session. Read this together with
> `KERFUS_PROJECT_STATE.md` (detailed state), `KERFUS_NEXT_STEPS.md` (roadmap),
> and `KERFUS_EMOTION_RUNTIME.md` (emotional architecture).
> Last updated: 2026-06-11.

---

## 1. What Kerfus is

A tiny **emotional social companion** device: an OLED face that reacts
contextually to touch/petting, motion, phone notifications, battery/power, and —
crucially — to **other Kerfus devices nearby**. Product philosophy:
**"First Kerfus meet each other. Then people meet each other."** It must feel
alive, not be a notification screen, Tamagotchi clone, random-animation player,
or BLE tracker. Read these for intent (all complete and consistent): `CLAUDE.md`
(agent-facing spec), `01_PRODUCT_VISION.md`, `02_…ARCHITECTURE.md`, and the
Russian `Master_goal_kerfur_russian.md`. `CLAUDE.md` is the most operational.

Hardware target: nRF52840 (`nice_nano`), Zephyr/NCS, SSD1306 128x64 mono OLED
(I2C 0x3c), LSM6DSO-class IMU, flex-PCB touch, 18650 cell, BLE.

> Naming note: code/BLE/Kconfig say **Kerfur**; the task prompt and these doc
> filenames say **Kerfus**. Same product. Don't "fix" the code spelling without
> the owner's say-so.

## 2. What the codebase contains (and it's a lot)

Mature, event-driven firmware (~16.5k LOC of project code on top of a vendored
Zephyr 4.4.99 + west modules). The full target architecture from `CLAUDE.md`
already exists:

```
Drivers → Input modules → Event bus → Behavior("emotion") engine
        → Behavior decision → Output (face / BLE / nearby beacon)
```

Working subsystems: event bus, behavior engine (9 internal drives, decay,
cooldowns, mode/expression/intent resolution), micro-reactions, LVGL face
pipeline with build-time codegen, display policy, motion stack (wake/walk/steps/
pickup/in-hand/tilt-gaze/shake), touch, BLE peripheral (bonding, ANCS, ANS,
optional companion/keyring/RSC), Kerfur↔Kerfur nearby (ephemeral-ID beacon +
scanner + peer state machine + encounter log + social events), and a rich
`kerfur` shell for diagnostics/injection.

## 3. Where the important files are

| Concern | File |
|---------|------|
| Spec / product law | `CLAUDE.md` |
| Build + shell reference | `README.md` |
| Main loop / event routing | `src/app/app.c` |
| Event types & payloads | `include/core/app_event.h` |
| Event bus | `src/core/event_bus.c` |
| Brain ("emotion engine") | `src/behavior/behavior_engine.c`, `include/behavior/pet_state.h` |
| Emotion architecture doc | `docs/KERFUS_EMOTION_RUNTIME.md` |
| Emotional memory (persistence) | `src/behavior/emotion_memory.c`, `include/behavior/emotion_memory.h` |
| Face render | `src/ui/face_runtime.c`, `src/ui/ui_renderer.c`, `assets/face/kerfur_faces.json` |
| Face codegen | `tools/face_codegen.py` (runs from `CMakeLists.txt`) |
| Display states | `src/display/display_policy.c` |
| Motion | `src/drivers/motion_*.c`, `in_hand_detector.c` |
| Power (timeouts only) | `src/power/power_manager.c` |
| BLE | `src/ble/ble_manager.c`, shell in `src/ble/ble_shell.c` |
| Nearby | `src/nearby/kerfur_nearby.c`, `encounter_log.c` |
| Feature switches/tuning | `Kconfig`, `prj.conf`, `prj_release.conf` |
| Board defs | `boards/nice_nano.overlay`, `boards/nice_nano_usb.overlay`, `boards/shields/kerfur_oled/` |

Ignore `zephyr/` and most of `modules/` — vendored upstream.

## 4. How to build / run / test

```bash
# Toolchain: Zephyr SDK 1.0.1 at C:\Users\Alexandr\zephyr-sdk-1.0.1
#   (do NOT reuse build_codex/ — its cache pins the broken 0.16.5 path)
# From repo root, ZEPHYR_BASE=<repo>/zephyr

# Dev build (USB CDC shell + BLE + nearby + motion; USB overlay auto-added):
west build -p always -b nice_nano -d build/nice_nano_dev -- -DSHIELD=kerfur_oled

# Release build:
west build -p always -b nice_nano -d build/nice_nano_release -- -DSHIELD=kerfur_oled -DCONF_FILE=prj_release.conf

west flash       # output UF2 at build/<dir>/zephyr/zephyr.uf2
```
Face codegen needs Python `Pillow` + (`cairosvg` or local Chrome).
**Tests:** none yet for project code (see NEXT_STEPS M3).
**Run without hardware:** set `CONFIG_KERFUR_ENABLE_MOCK_INPUTS=y` and/or drive
the `kerfur` shell (`face …`, `nearby inject …`, battery injection).

## 5b. What changed 2026-06-12 — Kerfus-to-Kerfus social system rewrite

Goal: the old nearby subsystem tied friend identity to the ephemeral ID in use right
now, so every ID rotation (every ~15 min) silently broke friend recognition —
same physical device appeared as a new stranger after each rotation or reboot.

**Core change: IRK-style friend recognition across ID rotation.**

Friends are now recognized cryptographically, not by ID matching.  On the bench:

- Each device generates and persists a `device_secret` (32-byte random, NVS).
- Confirmed friends share their `device_secret` via the companion app.
- Ephemeral IDs are now `crc32(peer_key || wall_clock_slot)` — predictable by
  any device that holds the `peer_key` for a given time slot.
- Both devices must have wall-clock time synchronized (via `APP_EVENT_TIME_SYNC`
  from the companion app) so their slot numbers match.  Without a clock,
  recognition degrades gracefully to "curious stranger" — not an error.

**Files changed:**

| File | What changed |
|------|--------------|
| `Kconfig` | `KERFUR_NEARBY_MAX_FRIENDS` (0–8, default 8) |
| `include/nearby/kerfur_nearby.h` | `struct kerfur_friend_record`, new friend API, `friend_index` in `kerfur_peer`, `kerfur_nearby_set_wall_clock()` |
| `include/core/app_event.h` | `int8_t friend_index` added to `struct app_event_peer` |
| `include/behavior/pet_state.h` | `int8_t current_active_friend_index` in `pet_state` |
| `include/nearby/encounter_log.h` | `int8_t friend_index` in `encounter_record`; updated `encounter_log_begin()` sig |
| `src/nearby/encounter_log.c` | `encounter_log_begin()` stores `friend_index` |
| `src/nearby/kerfur_nearby.c` | Full rewrite: NVS friend persistence, IRK resolution, wall-clock slots, rotation continuity, deferred NVS writes |
| `src/behavior/behavior_engine.c` | `current_active_friend_index = -1` in init; PEER_NEAR/PEER_LOST/ENCOUNTER_START/ENCOUNTER_END use `friend_index` for stable identity |
| `src/app/app.c` | `APP_EVENT_TIME_SYNC` → `kerfur_nearby_set_wall_clock()` |
| `src/ble/ble_shell.c` | `kerfur nearby friends {dump,resolve,add,remove,test_rotate}` and `kerfur nearby peers` |

**What now happens when a confirmed friend rotates ephemeral ID mid-encounter:**

1. BLE scan sees new ephemeral ID.
2. `resolve_friend_id()` tests the new ID against ±1 wall-clock slots for each
   stored friend key.
3. Match found → `peer_find_friend_locked(friend_index)` locates the existing
   peer table entry.
4. `peer->ephemeral_id` updated silently to the new ID.
5. No `PEER_LOST` or `ENCOUNTER_END` fires.  The emotion engine sees no break.

**Behavior engine stability across rotation:**

`state->current_active_friend_index` (int8_t, `-1` = none) is the stable
identity.  `PEER_LOST` / `ENCOUNTER_END` compare `friend_index` first, falling
back to `ephemeral_id` only for non-friend peers.

**Privacy model (unchanged at the wire level):**

- Only ephemeral IDs are ever broadcast over-the-air.
- `device_secret` / `peer_key` are stored only in NVS, never transmitted in
  beacons.
- Without wall-clock sync a device cannot predict anyone's ID → acts as an
  anonymous stranger (same as before this change).
- Persistent friend recognition requires: (a) explicit companion-app pairing,
  (b) wall-clock sync on both devices.  No hidden tracking is possible.

**Known limitation:**  If neither device has ever connected the companion app,
`g_wallclock_valid` remains false and friends appear as curious strangers.
This is the correct privacy-safe degradation.  First time-sync after app
connection triggers an immediate ID rotation to the wall-clock slot and enables
recognition within one slot period.

**Not built in this session** (owner builds manually).  Code was traced
line-by-line.  Build commands unchanged.

## 5a. What changed 2026-06-12 — scoring + motion production rework

Owner direction: the old expression scoring had production-grade flaws
(incommensurate scales, magic numbers in a switch, no notion of situation),
and the motion stack did not understand the real product: a **keychain worn
on jeans/backpack** that must still recognize being taken into the hand.
Both were reworked (clean-break approved; built nothing — owner builds):

- **Hybrid context + table scoring.** New `src/behavior/appraisal.c`:
  normalized 0..100 feature vector (28 features) × one calibrated weight
  table per expression; deterministic situation layer
  (CHARGING > SOCIAL > ENGAGED > WORN_QUIET/WORN_LIVELY > RESTING) adds
  per-situation biases and allowed-expression masks. The 170-line
  `score_for_expression()` switch is gone; intent alignment, transition
  affinity, hysteresis and the petting/asleep overrides remain in the
  engine. All scoring tuning now lives in `appraisal.c`.
- **Carry context** (`enum pet_carry_context` in pet_state, resolved by the
  motion stack, published as `APP_EVENT_CARRY_CONTEXT_CHANGED` + extended
  carry payload): ON_SURFACE / IN_HAND / WORN / TRANSITION / UNKNOWN.
- **WORN detection** (`in_hand_detector.c`, heavily reworked): worn evidence
  accumulator fed by gait cadence + new swing-periodicity feature
  (`compute_swing_score` — zero-crossing regularity of gravity-projected
  bounce); sticky while the owner sits still; exits to surface/in-hand only.
  **Grab capture**: while worn, pickup/in-hand evidence is gated behind the
  dangle-oscillation-collapse signature, so body bounce can never become a
  phantom pickup but a real grab is recognized in ~1–2 s.
- **Worn sanity**: SHAKE_LIGHT/PLAY never fire worn; ROUGH/IMPACT need
  ~1.4× energy; MOTION_WAKE app events suppressed while worn (wake line
  still opens short evaluation bursts); new `MODE_WORN_WATCH` classifier
  mode (150 ms heartbeat, hw step counter alive) replaces the old behavior
  of burning full-rate active windows on every step.
- **Worn style configurable** (owner chose "both modes"):
  `CONFIG_KERFUR_WORN_EXPRESSIVE` default + `kerfur emotion worn
  <quiet|expressive>` persisted in emotional memory (flags byte, layout
  unchanged). Quiet (default): display rests, motion/notification wakes and
  idle quirks suppressed, steps/peers/grabs/touch/charger still act.
  New `PET_MODE_WORN` (beacon value 9, append-only).
- Display policy worn handling; `kerfur face carry … [ctx]` injection;
  emotion dump now prints carry context + situation + worn style.
- **Weights are calibrated, not guessed**: `tools/appraisal_calibrate.py`
  mirrors the integer scoring exactly and asserts 20 canonical scenarios
  + 7 anti-flapping stability checks (all green). The baked table fixes a
  spec violation (friend encounter now greets HAPPY, not PLAYFUL) and an
  old volatility hole (transition affinity made adjacent flips cheaper
  than no hysteresis; a +3 incumbency bonus in `update_expression`
  restores the balance). Tune the script first, then bake.
- Docs: `KERFUS_EMOTION_RUNTIME.md` §1a/§1b (situation/appraisal + carry),
  README updated. All new code line-by-line traced, not compiled.
- **Known limitations for the next agent**: grab-while-walking is detected
  late (capture needs the swing to collapse — usually when the owner stops
  or holds it steady); boards without a hw step counter lose sw step
  detection in `MODE_WORN_WATCH` (low sample rate); swing detection
  freezes (decays) during worn-watch and re-measures during bursts. The
  worn thresholds (`WORN_*`, `GRAB_*`, `SWING_*`) need on-body tuning.

## 5. What changed in this session (2026-06-11 — emotion runtime rework)

Goal: make Kerfus feel genuinely alive, contextual, and emotionally present.
Full description in `docs/KERFUS_EMOTION_RUNTIME.md`. Summary of what is now
alive/contextual:

- **Mood axis** (`pet_state.mood`, 0–100): slow day-scale valence fed by a
  rate-limited accumulator (±2/min max), drifting home over hours; tilts
  expression scoring, PLAY/WITHDRAW intent, and the resting face.
- **Emotional memory** (`src/behavior/emotion_memory.c`, new module,
  `CONFIG_KERFUR_EMOTION_MEMORY`): attachment/trust/mood/lifetime-pets/
  personality persist across reboots in one settings/NVS record; throttled
  saves (~30 min, plus charger-connect, plus immediate on personality change).
- **Personality profiles** (balanced/curious/shy/playful/calm) scaling touch
  warmth, stress sensitivity, social appetite, playfulness, tiredness; new
  event `APP_EVENT_PERSONALITY_SET` + shell `kerfur emotion personality`.
- **Peer emotional contagion**: PEER_NEAR / ENCOUNTER_START now read the
  peer's broadcast mode/expression and classify a vibe (resting/bright/
  strained) — quiet greeting for a sleeping Kerfur, shared excitement with a
  happy one, concern for a strained/lonely one. Shell injects accept an
  optional `[expr]` arg; default = IDLE/CALM peer.
- **Sleep inertia**: 45 s grogginess after deep-sleep wake (90 s at night):
  sleepy bias, REST intent bonus, happy-bounce suppressed. Night WAKE is more
  reluctant. IDLE/AMBIENT timeouts now settle arousal/stress.
- **Idle micro-life (behavior)**: intent-driven idle gestures every 25–70 s
  (look up when attention-seeking, glances when curious, slow blink when
  resting) — only while the screen is on, never on low battery.
- **Face runtime "Stage 5" organic micro-life**: emotion-aware jittered blink
  cadence + occasional double blinks + post-reaction settle blinks; wandering
  idle gaze (random pupil drift-hold-return) which also finally applies the
  recipes' ambient pupil drift (it was computed but dead before); 1 px
  breathing bob on mouth/whiskers (continues during sleep); mood/arousal/
  sleepiness bias on openness/brows/mouth.
- **Diagnostics**: `kerfur emotion dump` (CLAUDE.md §15 "emotion print"),
  arousal/social_load/mood/personality added to the heartbeat status dump.
- Docs updated: `README.md` (features, architecture, Kconfig, shell),
  new `docs/KERFUS_EMOTION_RUNTIME.md`, this handoff, NEXT_STEPS statuses.
- **Not built in this session** (owner builds manually): code was
  line-by-line traced instead. Build commands unchanged (see §4); the only
  new source file is `src/behavior/emotion_memory.c` (CMake-gated on
  `CONFIG_KERFUR_EMOTION_MEMORY`, default y, depends on `SETTINGS`).

Earlier session (2026-06-09): repo audit, created PROJECT_STATE / NEXT_STEPS /
this handoff; pristine verification build **SUCCESS** (SDK 1.0.1,
`nice_nano/nrf52840`, FLASH 72.3% / RAM 31.5%) — pre-rework numbers.

## 6. What must NOT be broken

- The event-bus contract in `include/core/app_event.h` (everything depends on it;
  note the backward-compat alias `#define`s at the bottom).
- The face codegen flow: `assets/face/kerfur_faces.json` → `tools/face_codegen.py`
  → checked-in `*/generated/*` — regenerate, don't hand-edit generated files.
- Hardware-graceful degradation: firmware must still build/run if `motion0` or
  `touch0` aliases are absent.
- Privacy rule for nearby: ephemeral rotating IDs by default; persistent identity
  only via explicit future-app pairing. Never turn it into a tracker.
  (Contagion only reads the anonymous beacon summary already broadcast.)
- Keep emotional logic out of drivers and BLE out of emotion decisions
  (`CLAUDE.md` §16).
- Emotion-runtime invariants (`KERFUS_EMOTION_RUNTIME.md` §10): mood changes
  only via the rate-limited accumulator; personality IDs and the
  emotion-memory record layout are persisted (append-only, version-bump on
  change); idle micro-life stays subtle and battery-aware.

## 7. What the next agent should do first

1. Read `CLAUDE.md`, then `KERFUS_PROJECT_STATE.md`, then
   `KERFUS_EMOTION_RUNTIME.md`.
2. Verify the 2026-06-11 emotion rework builds clean (owner builds manually;
   code was traced but not compiled in that session) and watch the new
   runtime logs: `Emotional memory restored/saved`, `Groggy wake-up`,
   `Encounter start: … vibe=…`, `Emotion dump …`.
3. Resolve open questions in §8 with the owner.
4. Then the remaining "living device" gaps: real charger/fuel-gauge wiring on
   hardware (M1 — `battery_monitor.c` + BQ25630 backend now exist), true
   sleep (M2), and behavior/nearby unit tests (M3) — the mood/contagion/
   memory logic is pure enough to unit-test off-target.

### What should be improved next (emotion-specific)

- Unit tests for: mood accumulator rate limiting, peer-vibe classification,
  grogginess window, emotion-memory record round-trip.
- Let personality also bias idle-quirk cadence and blink/wander tuning (face
  currently reads only drives/mood, not the profile).
- Persist known/friend peers (L4) so contagion can deepen with relationship
  history ("this friend is usually sleepy in the morning").
- Hook `energy` to the real battery percent once M1 hardware lands (today
  energy is display/charging-driven; battery flags arrive via events).
- Optional: quiet hours from the companion app should damp notification
  arousal/stress at night beyond the built-in night handling.

## 8. Open questions (need owner input)

- **Kerfus vs Kerfur** — canonical name? Docs say Kerfus, code says Kerfur.
  Rename code to match, or align docs to the code? (No action taken.)
- **Battery hardware** — which pin/peripheral? ADC channel + divider for the
  18650, and is there a charger STAT/charge-detect GPIO? (Blocks M1.)
- **Drive vocabulary** — adopt code names (energy/attachment/…) or `CLAUDE.md`
  names (mood/affection/…) as canonical?

## 9. Hardware assumptions

- nRF52840 on `nice_nano`; `kerfur_oled` shield (SSD1306 128x64 @ I2C 0x3c).
- `motion0` = LSM6DSO-class IMU; `touch0` = flex-PCB touch alias.
- External VCC rail kept enabled in `boards/nice_nano.overlay`.
- 18650 single cell; **no battery-sense or charger driver wired yet** — events
  exist but are mock-only.
- `nrf52840dk_nrf52840.overlay` is an alternate dev board example.

## 10. Software architecture direction

Keep it modular and event-driven; grow by adding modules behind the event bus,
not by enlarging the main loop. New inputs publish events; new outputs subscribe.
Battery/power, persistence/personality, and companion-API are the next modules to
add. Reconcile the doc/code drive-name drift, add tests around the pure state
machines, and only refactor `pet_state` (the central coupling point) with care
and a written rationale.
