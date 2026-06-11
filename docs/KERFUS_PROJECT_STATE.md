# Kerfus / Kerfur — Project State Report

> Audit date: 2026-06-09
> Auditor: Claude (software/firmware agent)
> Scope: repository at `c:\Users\Alexandr\kerfur`, branch `master`

This file is the canonical "where are we now" snapshot. Future Claude / Codex /
ChatGPT sessions should read this **first**, then `KERFUS_AGENT_HANDOFF.md` and
`KERFUS_NEXT_STEPS.md`.

> **Addendum 2026-06-11 — emotion runtime rework.** The behavior engine and
> face runtime were significantly extended after this audit: mood axis,
> emotional memory persistence (`src/behavior/emotion_memory.c`,
> `CONFIG_KERFUR_EMOTION_MEMORY`), personality profiles, peer emotional
> contagion, sleep inertia, idle micro-life, organic blinking / wandering
> gaze / breathing on the face, and `kerfur emotion` shell commands. See
> `KERFUS_EMOTION_RUNTIME.md` (architecture) and `KERFUS_AGENT_HANDOFF.md` §5
> (change list). Battery monitor scaffolding (`src/power/battery_monitor.c`,
> BQ25630 backend) and the IQS7222A touch driver also landed between the
> audit and the rework. The build-status numbers in §6 are pre-rework; the
> rework session itself did not run a build (owner builds manually).

---

## 1. Executive summary

Kerfur is **not** an early prototype. It is a fairly mature, well-architected,
event-driven Zephyr/NCS firmware for an nRF52840 "smart pet" companion. The
conceptual architecture demanded by `CLAUDE.md`
(Drivers → Input → Event Bus → Emotion/Behavior → Behavior Decision → Output) is
**already implemented** and, in several areas, exceeded.

Product intent is well documented: `01_PRODUCT_VISION.md`,
`02_SOFTWARE_REQUIREMENTS_AND_FIRMWARE_ARCHITECTURE.md`,
`Master_goal_kerfur_russian.md` (a Russian-language master-goal doc), `CLAUDE.md`,
and `README.md` are all populated and mutually consistent. The vision docs use
the product name **"Kerfus"** while the code/BLE/Kconfig use **"Kerfur"** — see §5.

(Audit note: the three vision docs read as 0 bytes during the first pass of this
audit; they were simply unsaved at that moment and are now complete.)

The main *engineering* gaps are real-hardware battery/charger sensing and actual
low-power (sleep) entry — the behavior and face layers already consume those
events, but no driver produces them yet.

---

## 2. Repository structure (project-relevant only)

The repo is a **Zephyr T2 freestanding application that vendors the full Zephyr
tree and west modules inside itself**. Ignore `zephyr/` and `modules/` (except
`modules/hal/espressif`, which is the only locally-modified submodule) — those
are the upstream RTOS and HALs.

```
kerfur/
├── CLAUDE.md          # Agent-facing product + architecture spec (authoritative)
├── README.md          # Accurate, detailed build/architecture/shell reference
├── 01_PRODUCT_VISION.md                              # Product vision (Kerfus)
├── 02_SOFTWARE_REQUIREMENTS_AND_FIRMWARE_ARCHITECTURE.md  # SW reqs + architecture
├── Master_goal_kerfur_russian.md                     # Russian master-goal doc
├── CMakeLists.txt     # App build + face codegen custom command
├── Kconfig            # All KERFUR_* feature switches + tuning
├── prj.conf           # Dev profile (USB shell, BLE, ANCS, nearby, motion)
├── prj_release.conf   # Quieter release profile (no USB, no logging)
├── assets/face/       # kerfur_faces.json + SVG — source of truth for the face
├── boards/            # nice_nano (primary), nrf52840dk overlays, kerfur_oled shield
├── dts/               # local devicetree bindings
├── tools/face_codegen.py   # JSON/SVG → generated C (runs from CMake)
├── include/           # public headers, mirrors src/ layout
│   ├── app/ behavior/ ble/ core/ display/ drivers/ nearby/ power/ ui/
│   └── ui/generated/  # generated face headers (checked in)
├── src/               # ~16.5k LOC of project code
│   ├── main.c                  # thin: calls app_run()
│   ├── app/app.c               # main loop + event fan-out
│   ├── core/event_bus.c        # Zephyr msgq event bus
│   ├── behavior/               # behavior_engine.c, micro_reaction.c  (the "emotion engine")
│   ├── display/display_policy.c
│   ├── ui/                     # ui_renderer.c, face_runtime.c, generated/
│   ├── drivers/                # motion_sensor, motion_classifier, in_hand_detector,
│   │                           #   touch_input, mock_inputs
│   ├── power/power_manager.c   # idle/sleep TIMEOUT state machine (no HW battery yet)
│   ├── ble/                    # ble_manager.c, ble_shell.c
│   └── nearby/                 # kerfur_nearby.c, encounter_log.c
├── docs/
│   ├── gadgetbridge/  # Android companion protocol notes + scaffold
│   └── KERFUS_*.md    # THIS audit set (new)
├── build/             # build outputs (audit build here)
└── build_codex/       # a previous successful build (SDK 0.16.5 pin — now stale)
```

Untracked working-tree items at audit time: `case and pcb/` (mechanical design,
not firmware) and a modified `modules/hal/espressif` submodule pointer.

---

## 3. What already exists and works (by subsystem)

| Subsystem | File(s) | State |
|-----------|---------|-------|
| App core / main loop | `src/app/app.c`, `src/main.c` | **Working.** Event-drain loop (budget 32), frame pacing via `KERFUR_UI_FRAME_MS`, fans every event to behavior → power → display → motion. |
| Event bus | `src/core/event_bus.c`, `include/core/app_event.h` | **Working.** Central Zephyr msgq; ~45 typed event types; typed payloads (look target, carry state, step batch, battery %, peer). Publish helpers with/without timestamp. |
| Behavior engine ("emotion engine") | `src/behavior/behavior_engine.c` | **Working & rich.** `pet_state` tracks 9 internal 0–100 variables: energy, sleepiness, attachment, boredom, stress, arousal, social_load, trust, curiosity. Resolves pet *mode*, *expression*, *intent*, *micro-reaction* from events + passive time drift. |
| Micro-reactions | `src/behavior/micro_reaction.c` | **Working.** Short-lived reaction layer with cooldowns. |
| Face pipeline | `src/ui/face_runtime.c` (1518 LOC), `ui_renderer.c`, generated assets/recipes | **Working.** LVGL canvas on 128x64 mono SSD1306; recipes, blink, pupil/gaze movement, indicators, overlays, effects, transitions. Assets generated at build time from `assets/face/kerfur_faces.json`. |
| Display policy | `src/display/display_policy.c` | **Working.** Foreground / ambient / off states, contrast, ambient pixel shift, self-wake scheduling. |
| Motion stack | `src/drivers/motion_sensor.c`, `motion_classifier.c`, `in_hand_detector.c` | **Working** for `st,lsm6dso` on `motion0` alias. Wake-on-motion/tilt, walking confidence + step batching, pickup/in-hand detection, tilt→gaze, shake/impact classification. Degrades gracefully if `motion0` absent. |
| Touch | `src/drivers/touch_input.c` | **Present.** `touch0` alias-driven tap/pet/hold gestures feeding events. |
| Mock inputs | `src/drivers/mock_inputs.c` | **Present** (disabled by default in `prj.conf`). Periodic synthetic events for sensorless bring-up. |
| BLE | `src/ble/ble_manager.c` | **Working scaffold.** Peripheral advertising, directed reconnect for bonded peers, security/bonding (SMP + settings/NVS), ANCS intake (iOS), ANS fallback, optional companion RX/TX, optional keyring + RSC services. |
| Nearby (Kerfur↔Kerfur) | `src/nearby/kerfur_nearby.c` (783 LOC), `encounter_log.c` | **Working.** Manufacturer-data beacon (`KFR` magic) in scan response, ephemeral rotating IDs, duty-cycled scanner, peer state machine (NONE/SEEN/NEAR/INTERACTING/COOLDOWN), RSSI hysteresis, encounter log ring buffer, social events (greet/play invite/ack/bye), friend list helpers. Privacy-conscious (ephemeral IDs by default). |
| Diagnostics / shell | `src/ble/ble_shell.c` | **Working.** `kerfur` root command: `ble …`, `face …` (expr/react/look/carry/battery/motion injection), `nearby inject …`. Matches the diagnostics requirement in `CLAUDE.md` §15. |

### Mapping to the CLAUDE.md target architecture

```
Drivers (motion_sensor, touch_input, ble_manager, SSD1306, mock_inputs)
   ↓
Input Modules (motion_classifier, in_hand_detector, touch gestures, nearby scan)
   ↓
Event Bus (core/event_bus.c)
   ↓
Emotion Engine == behavior_engine.c + pet_state (9 drives, decay, cooldowns)
   ↓
Behavior Decision (mode/expression/intent/micro-reaction resolution)
   ↓
Output Modules (display_policy → ui_renderer → face_runtime; ble; nearby beacon)
```

The required architecture exists. The "emotion engine" is named **behavior
engine** here; its drive variables differ in naming from `CLAUDE.md` §7
(attachment≈affection, social_load≈social_interest, arousal≈alertness,
boredom/trust added) but the concept and contextual (non-random) reaction rule
are honoured.

---

## 4. What does not work yet / is incomplete

1. **Real battery + charger sensing** — *highest-value gap.* `power_manager.c`
   only runs idle/sleep timeouts; it never reads a fuel gauge or charge-detect
   GPIO. `APP_EVENT_BATTERY_LOW/CRITICAL/PERCENT_UPDATE` and
   `CHARGER_CONNECTED/DISCONNECTED` are **only produced by mock/shell injection**,
   yet behavior + face + nearby-status already consume them. 18650 cell per spec.
2. **Actual low-power entry** — `APP_EVENT_SLEEP_REQUEST` is emitted and
   acknowledged, but nothing calls Zephyr PM / configures wake sources. The
   device does not truly sleep yet.
3. **Android companion payload parsing** — companion RX/TX characteristics exist
   but only the compact category-based protocol is parsed; richer payloads and
   the Gadgetbridge notification-enable helper are unfinished (see
   `docs/gadgetbridge/`).
4. **No automated tests** — there are no project `testcase.yaml` / ztest / native
   unit tests for the behavior or nearby logic (the many `testcase.yaml` hits are
   inside vendored `modules/`). The behavior and nearby state machines are pure
   enough to be unit-testable but currently are not.
5. **Persistence/personality** — bonding keys persist via BT settings/NVS, but
   the `CLAUDE.md` §14 "memory/personality" store (device name, personality
   selection, affection, known/friend peers across reboot) is not yet a module.

---

## 5. What is unclear / assumptions made

- **Naming: "Kerfus" vs "Kerfur".** The product/vision docs and these doc
  filenames say *Kerfus*; the entire codebase, BLE device name, and
  `CONFIG_KERFUR_*` say *Kerfur*. They are the same product. I left all code as
  `Kerfur` and the docs as authored. Open question for the owner: is `Kerfus` the
  canonical brand (rename code later) or is the doc spelling to be aligned to the
  code? No action taken either way.
- Product spec sources are consistent: `CLAUDE.md`, `01_PRODUCT_VISION.md`,
  `02_…ARCHITECTURE.md`, and the Russian `Master_goal_kerfur_russian.md` all
  describe the same modular, event-driven, emotion-first companion. Where the
  docs name an "Emotion Engine", the code realizes it as the behavior engine.
- Primary HW target assumed to be `nice_nano` (nRF52840) + `kerfur_oled` shield,
  per `README.md`, `build_codex` build info, and `prj.conf`. Vision docs add a
  display detail: 0.96" SSD1306, ~24.7 mm active length, with the case face
  opening sized to the display.

---

## 6. Build status

- **Toolchain present:** Zephyr SDK at `C:\Users\Alexandr\zephyr-sdk-1.0.1`
  (the cache in `build_codex` pins the older **0.16.5**, whose path no longer has
  a working `arm-zephyr-eabi-gcc`). `ninja`, `cmake 4.1.2`, Python 3.13 available.
  West v1.3.0; Zephyr 4.4.99 in-tree.
- **A prior build succeeded** (`build_codex/`, board `nice_nano`/nrf52840) — proof
  the codebase compiles and links.
- **Incremental rebuild of `build_codex` fails** purely because of the stale SDK
  0.16.5 pin in its CMake cache (gcc not found at that path). This is an
  environment/cache issue, **not** a code defect.
- **Pristine rebuild** with the available SDK 1.0.1 was launched into
  `build/audit_nice_nano` during this audit:
  `west build -p always -b nice_nano -d build/audit_nice_nano -- -DSHIELD=kerfur_oled`.

  <!-- BUILD_RESULT -->
  **Result: SUCCESS (exit 0, 2026-06-09).** Clean build with SDK 1.0.1, board
  `nice_nano/nrf52840`, `-DSHIELD=kerfur_oled`. Produced `zephyr.elf/.hex/.uf2`.
  Memory: **FLASH 586,508 B / 792 KB = 72.3%**, **RAM 82,648 B / 256 KB = 31.5%**.
  UF2 = 1,173,504 B, start 0x26000. The codebase compiles, links, and fits the
  nRF52840 with comfortable headroom. (Note FLASH is ~72% with the full dev
  profile — USB+shell+BLE+ANCS+nearby+LVGL+mbedTLS; watch this as features grow.)

### Build commands of record (from README, verified shapes)

```bash
# Dev (USB shell + BLE + nearby + motion). USB overlay auto-appended for nice_nano.
west build -p always -b nice_nano -d build/nice_nano_dev -- -DSHIELD=kerfur_oled

# Release (no USB, no logging)
west build -p always -b nice_nano -d build/nice_nano_release -- -DSHIELD=kerfur_oled -DCONF_FILE=prj_release.conf
```

Face codegen needs Python `Pillow` and either `cairosvg` or a local Chrome for
SVG rasterization.

---

## 7. Architecture status

Healthy. Clean module boundaries, hardware isolated from emotional logic, a real
event bus, explicit state machines (pet mode, peer state, display policy, power
timeouts). No "one giant loop with all logic inside" — `app.c` only routes
events. This matches `CLAUDE.md` §5/§16 well.

---

## 8. Detected technical debt

- Face metadata glue is partly hand-maintained vs. partly generated
  (README "Open Work" notes moving the rest into codegen).
- `pet_state` is a large flat struct (~50 fields). Acceptable for embedded, but
  it is the central coupling point; changes ripple widely.
- Drive-variable naming diverges from the doc vocabulary (`CLAUDE.md` §7 /
  `02_…` §4.3 say mood/affection/social_interest/alertness/tiredness; code uses
  energy/sleepiness/attachment/boredom/stress/arousal/social_load/trust/curiosity)
  — worth reconciling so docs and code share one vocabulary.
- Two stale build trees (`build_codex`, `build/nearby_test`) committed-adjacent;
  the 0.16.5 cache misleads anyone running an incremental build.

---

## 9. Immediate risks

- **Doc/code drift:** the vision docs are now complete and consistent, but they
  use different drive-variable names and the product name "Kerfus" vs the code's
  "Kerfur". Reconcile vocabulary/naming so future agents don't re-derive wrongly.
- **Battery feature is a phantom:** every layer reacts to battery events that
  nothing real emits — easy to mistake the mock behavior for working hardware.
- **No tests:** behavior/nearby regressions are invisible until on-device.
- **SDK version drift** (0.16.5 cache vs 1.0.1 installed) will bite anyone who
  runs a non-pristine build.

---

## 10. Recommended next steps (summary — full detail in KERFUS_NEXT_STEPS.md)

1. Decide the canonical name (Kerfus vs Kerfur) and reconcile drive vocabulary.
2. Add a real battery/charger driver module behind the existing events.
3. Implement true low-power sleep on `SLEEP_REQUEST` with motion/touch wake.
4. Add native/ztest unit tests for `behavior_engine` and `kerfur_nearby` state
   machines (they are pure enough to test off-target).
5. Add a persistence/personality module (`CLAUDE.md` §14).
6. Pin one SDK version and document it; clean stale build trees.
