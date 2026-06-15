# Kerfur nRF52840 Firmware (Zephyr / NCS)

Kerfur is an event-driven "smart pet" firmware base for nRF52840 boards running Zephyr/Nordic Connect SDK. The current codebase is no longer just a minimal display demo: it now includes a behavior engine, a generated face rendering pipeline, motion sensing, BLE phone integration, and Kerfur-to-Kerfur nearby detection.

## Current Capabilities

- Event-driven application core with a central Zephyr message-queue event bus
- Behavior engine with pet modes, a deterministic situation layer (resting / engaged / worn / charging / social), normalized table-driven expression appraisal, short-lived context/afterglow, micro-reactions, a slow mood axis, sleep inertia, idle micro-life, and personality profiles
- Carry-context motion stack for a worn keychain: ON_SURFACE / IN_HAND / WORN resolution with swing-periodicity worn detection, grab-capture in-hand recognition, worn-gated shake events, and a low-power worn-watch sampling mode
- Emotional memory: attachment / trust / mood / lifetime petting count / personality / worn style persist across reboots (settings/NVS)
- Peer emotional contagion: reactions to other Kerfurs depend on the mode/expression carried in their beacon
- Modular face renderer driven by generated assets from `assets/face/kerfur_faces.json`
- LVGL canvas renderer for SSD1306-style 128x64 monochrome displays
- Display policy with foreground / ambient / off states, contrast changes, ambient pixel shift, and self-wake scheduling
- Motion stack for `motion0` IMU devices:
  - wake-on-motion / tilt
  - walking confidence and step batching
  - pickup and in-hand detection
  - tilt-driven look target updates for dynamic pupils
  - shake / impact classification
- BLE peripheral stack with:
  - directed reconnect for bonded peers
  - ANCS intake for iOS
  - ANS fallback discovery
  - optional custom companion channel for Android / Gadgetbridge
  - optional keyring profile services
  - optional Running Speed and Cadence service scaffold
- Kerfur-to-Kerfur nearby beaconing and scanning with ephemeral IDs, encounter tracking, and behavior events
- USB shell commands for BLE maintenance, face debugging, and nearby-event injection

## Default Build Profile

The checked-in `prj.conf` is a development-oriented profile. By default it enables:

- USB CDC ACM console + shell on `nice_nano`
- BLE peripheral + observer
- motion classifier
- ANCS
- face debug and face shell commands
- nearby detection
- mock inputs disabled

`prj_release.conf` keeps the same main firmware shape but disables USB device mode and logging for a quieter release build.

## Hardware Assumptions

- Primary board target: `nice_nano` from the local board definitions in this repo
- OLED shield: `kerfur_oled`
- Display: SSD1306-compatible 128x64 panel on I2C address `0x3c`
- Touch input exposed as the `touch0` devicetree alias
- Motion sensor exposed as the `motion0` devicetree alias
- Current motion tuning is centered around `st,lsm6dso`

Important board details:

- [boards/nice_nano.overlay](boards/nice_nano.overlay)
  - defines `touch0`
  - defines `motion0`
  - keeps the external VCC rail enabled on nice!nano-compatible hardware
- [boards/nice_nano_usb.overlay](boards/nice_nano_usb.overlay)
  - routes console and shell to USB CDC ACM
  - this overlay is appended automatically for non-release `nice_nano` builds
- [boards/shields/kerfur_oled/kerfur_oled.overlay](boards/shields/kerfur_oled/kerfur_oled.overlay)
  - provides the `zephyr,display` chosen node
- [boards/nrf52840dk_nrf52840.overlay](boards/nrf52840dk_nrf52840.overlay)
  - alternate board overlay example with SSD1306 + `motion0`

If `motion0` is not present, the firmware still builds and the motion stack stays disabled at runtime.

## Repository Layout

```text
.
|-- assets/
|   |-- face/                    JSON + SVG source for face assets/recipes
|   `-- face_visualizer/         local viewer for the face JSON
|-- boards/
|   |-- nice_nano.overlay
|   |-- nice_nano_usb.overlay
|   |-- nrf52840dk_nrf52840.overlay
|   `-- shields/kerfur_oled/
|-- docs/
|   `-- gadgetbridge/            Android companion notes and scaffold
|-- include/
|   |-- app/
|   |-- behavior/
|   |-- ble/
|   |-- core/
|   |-- display/
|   |-- drivers/
|   |-- nearby/
|   |-- power/
|   `-- ui/
|       `-- generated/           generated face headers
|-- src/
|   |-- app/
|   |-- behavior/
|   |-- ble/
|   |-- core/
|   |-- display/
|   |-- drivers/
|   |-- nearby/
|   |-- power/
|   `-- ui/
|       `-- generated/           generated face source
|-- tools/
|   `-- face_codegen.py          face JSON/SVG -> C generator
|-- CMakeLists.txt
|-- Kconfig
|-- prj.conf
`-- prj_release.conf
```

## Runtime Architecture

1. Event bus
- [src/core/event_bus.c](src/core/event_bus.c) owns the central app event queue.
- Time ticks, input, motion, BLE, nearby, and power all feed the same event pipeline.

2. Behavior engine
- [src/behavior/behavior_engine.c](src/behavior/behavior_engine.c) maintains long-lived pet state.
- It resolves pet mode, expression, intent, and micro-reactions from incoming events and passive time drift.
- The current state includes carry state, look target/render state, battery flags, walking data, and social state.
- Emotional layers, fastest to slowest: micro-reactions (seconds) → afterglow context (a minute) → drives (minutes) → mood (hours) → persisted traits (across reboots).
- A deterministic situation layer (charging > social > in-hand > worn > resting) selects how the emotional state may show; [src/behavior/appraisal.c](src/behavior/appraisal.c) then scores expressions from a normalized 0–100 feature vector against one calibrated weight table (all scoring tuning lives in that file).
- The weight table is baked from [tools/appraisal_calibrate.py](tools/appraisal_calibrate.py) — an executable mirror of the scoring that asserts 20 canonical scenarios and 7 anti-flapping stability checks (`python tools/appraisal_calibrate.py`). Edit the script, make it pass, then copy the numbers into `appraisal.c`.
- Carry context (`ON_SURFACE` / `IN_HAND` / `WORN`) comes from the motion stack: worn keychains are detected by gait-band swing periodicity, stay sticky while the owner sits still, and recognize a grab through the dangle oscillation collapsing (see [src/drivers/in_hand_detector.c](src/drivers/in_hand_detector.c)). Worn style is configurable: quiet companion (default — screen rests in the pocket, steps and peers still count) or expressive (`kerfur emotion worn …`, `CONFIG_KERFUR_WORN_EXPRESSIVE`).
- [src/behavior/emotion_memory.c](src/behavior/emotion_memory.c) persists the slow traits (attachment, trust, mood, lifetime pets, personality) in one settings/NVS record; saves are throttled (~30 min) plus on charger connect.
- Personality profiles (balanced / curious / shy / playful / calm) scale how strongly touch, stress, social events, play, and tiredness land — selectable at runtime via `kerfur emotion personality <id>` and persisted.
- Peer events carry the other Kerfur's mode/expression summary; the engine classifies the peer's "vibe" (resting / bright / strained) and greets accordingly: it doesn't bounce at a sleeping friend, lights up with a happy one, and shows concern for a strained or lonely one.
- Sleep inertia: leaving deep sleep keeps the pet groggy (sleepy expression bias, no happy-bounce) for 45 s (90 s at night).
- Idle micro-life: when nothing is happening and the screen is on, the pet occasionally glances around (curious), peeks up (wants attention), or slow-blinks (resting), every ~25–70 s.
- Expression transition routing: appraisal + hysteresis pick a stable target feeling, then the face walks toward it one emotional-adjacency hop at a time (CALM is the hub) so distant changes ease through intermediates instead of snapping — waking eases `ASLEEP → SLEEPY → …`, petting an angry pet soothes `ANNOYED → CALM → … → HAPPY`.
- See [docs/KERFUS_EMOTION_RUNTIME.md](docs/KERFUS_EMOTION_RUNTIME.md) for the full emotional architecture.

3. Motion stack
- [src/drivers/motion_sensor.c](src/drivers/motion_sensor.c) abstracts the IMU and runtime capabilities.
- [src/drivers/motion_classifier.c](src/drivers/motion_classifier.c) turns raw samples into motion events.
- [src/drivers/in_hand_detector.c](src/drivers/in_hand_detector.c) handles pickup / in-hand confidence and feeds look-target generation.

4. Display and face pipeline
- [src/display/display_policy.c](src/display/display_policy.c) drives foreground, ambient, and off transitions.
- [src/ui/ui_renderer.c](src/ui/ui_renderer.c) renders the face on an LVGL canvas and applies contrast / pixel shift policy.
- [src/ui/face_runtime.c](src/ui/face_runtime.c) composes recipes, reactions, blink state, pupil movement, indicators, overlays, and effects.
- Organic micro-life (Stage 5): emotion-aware randomized blink cadence with occasional double blinks and post-reaction settle blinks; an occasional slow wandering idle gaze (pupils ease to a random point, hold, return) when there is no real gaze input — the only idle pupil motion, the old constant cyclic drift was removed; a faint, slow breathing lift on mouth/whiskers that continues during sleep; mood/arousal/sleepiness bias eye openness, brows, and mouth.
- Smooth expression changes: on a change the eye whites (and the pupils that ride on them) glide to their new position instead of teleporting — brows, mouth, whiskers, eye openness and eye position all ease together.
- Face assets and recipes are generated during build from `assets/face/kerfur_faces.json` and the SVG sources in `assets/face/`.

5. BLE and nearby
- [src/ble/ble_manager.c](src/ble/ble_manager.c) owns advertising, connections, security, notification intake, and optional companion services.
- [src/nearby/kerfur_nearby.c](src/nearby/kerfur_nearby.c) builds Kerfur manufacturer-data beacons, scans peers, rotates ephemeral IDs, and emits social encounter events.
- [src/ble/ble_shell.c](src/ble/ble_shell.c) exposes the shell debug surface.

## Build

Prerequisites:

- Zephyr / NCS workspace with `west`
- Python 3 available to CMake
- Pillow installed for `tools/face_codegen.py`
- either `cairosvg` available to the same Python interpreter, or a local Chrome/Chromium install for SVG rasterization fallback

If needed, install the face codegen dependencies into the active interpreter:

```bash
python -m pip install --user Pillow cairosvg
```

Development build for `nice_nano`:

```bash
west build -p always -b nice_nano -d build/nice_nano_dev -- -DSHIELD=kerfur_oled
```

Release-oriented build for `nice_nano`:

```bash
west build -p always -b nice_nano -d build/nice_nano_release -- -DSHIELD=kerfur_oled -DCONF_FILE=prj_release.conf
```

Alternative board example:

```bash
west build -p always -b nrf52840dk_nrf52840 -d build/nrf52840dk -- -DDTC_OVERLAY_FILE=boards/nrf52840dk_nrf52840.overlay
```

Flash:

```bash
west flash
```

UF2 output for nice!nano-style builds is generated at:

```text
build/<build-dir>/zephyr/zephyr.uf2
```

## Main Kconfig Switches

The most important project-level switches live in [Kconfig](Kconfig):

- `CONFIG_KERFUR_ENABLE_BLE`
- `CONFIG_KERFUR_ENABLE_ANCS`
- `CONFIG_KERFUR_ENABLE_ANS_FALLBACK`
- `CONFIG_KERFUR_ENABLE_COMPANION`
- `CONFIG_KERFUR_ENABLE_KEYRING_PROFILE`
- `CONFIG_KERFUR_ENABLE_RSC_SERVICE`
- `CONFIG_KERFUR_ENABLE_NEARBY`
- `CONFIG_KERFUR_ENABLE_SHELL_CMDS`
- `CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS`
- `CONFIG_KERFUR_ENABLE_MOCK_INPUTS`
- `CONFIG_KERFUR_MOTION`
- `CONFIG_KERFUR_EMOTION_MEMORY`
- `CONFIG_KERFUR_WORN_EXPRESSIVE`
- `CONFIG_KERFUR_TRACE_EVENTS`
- `CONFIG_KERFUR_FACE_DEBUG`

Motion tuning and nearby thresholds are also configurable there.

## Shell Commands

When shell support is enabled, the firmware registers a `kerfur` root command.

BLE maintenance:

- `kerfur ble status`
- `kerfur ble adv_restart`
- `kerfur ble disconnect`
- `kerfur ble unpair_all`
- `kerfur ble gb_status`
- `kerfur ble gb_test [category]`

Face debugging:

- `kerfur face dump`
- `kerfur face look <x> <y> [confidence]`
- `kerfur face carry <in_hand> <pickup_conf> <in_hand_conf> <walking_conf>`
- `kerfur face battery <percent|unknown>`
- `kerfur face motion walk_start [confidence]`
- `kerfur face motion walk_stop [confidence]`
- `kerfur face motion step_batch <steps> [walking_conf]`
- `kerfur face motion dynamic_pupils <on|off>`
- `kerfur face motion conf_log <on|off>`
- `kerfur face expr list`
- `kerfur face expr set <name|id>`
- `kerfur face expr clear`
- `kerfur face react list`
- `kerfur face react trigger <name|id>`

Nearby debugging:

- `kerfur nearby inject seen <id>`
- `kerfur nearby inject checking <id>`
- `kerfur nearby inject near <id> [rssi] [expr]`
- `kerfur nearby inject friend <id> [expr]`
- `kerfur nearby inject unknown <id> [expr]`
- `kerfur nearby inject play_invite <id>`
- `kerfur nearby inject play_ack <id>`
- `kerfur nearby inject lost <id>`
- `kerfur nearby end <id> [duration_s]`

The optional `expr` argument (a `pet_expression` id, 0–12) simulates the peer's
broadcast expression so emotional contagion can be tested: e.g. `5` (SLEEPY)
yields a quiet greeting, `4` (PLAYFUL) an excited one, `9` (OVERSTIMULATED) a
concerned one.

Emotional state debugging (always available with shell support):

- `kerfur emotion dump` — print drives, mood, context, intent, grogginess, personality, lifetime pets, carry context, and situation to the log
- `kerfur emotion personality <0..4>` — switch personality (0 balanced, 1 curious, 2 shy, 3 playful, 4 calm); persisted via emotional memory
- `kerfur emotion worn <quiet|expressive>` — switch the worn style; persisted via emotional memory

Carry-context injection (face debug shell): `kerfur face carry <in_hand> <pickup> <inhand_conf> <walk_conf> [ctx]` where ctx is 0 unknown / 1 surface / 2 in-hand / 3 worn / 4 transition.

## Face Authoring Tooling

- JSON/SVG source of truth: `assets/face/kerfur_faces.json`
- Local face viewer: [assets/face_visualizer/README.md](assets/face_visualizer/README.md)
- Code generator: [tools/face_codegen.py](tools/face_codegen.py)

The generator runs automatically from CMake and writes:

- [include/ui/generated/kerfur_face_assets.h](include/ui/generated/kerfur_face_assets.h)
- [include/ui/generated/kerfur_face_recipes.h](include/ui/generated/kerfur_face_recipes.h)
- [src/ui/generated/kerfur_face_assets.c](src/ui/generated/kerfur_face_assets.c)
- [src/ui/generated/kerfur_face_recipes.c](src/ui/generated/kerfur_face_recipes.c)

## Tests

Two off-target test harnesses cover the two halves of the face/emotion runtime;
both run on a desktop without the Zephyr toolchain or hardware:

- **Engine selection** — [tools/appraisal_calibrate.py](tools/appraisal_calibrate.py)
  mirrors the integer appraisal scoring and asserts 20 canonical emotional
  scenarios + 7 anti-flapping stability checks. Run: `python tools/appraisal_calibrate.py`.
- **Face rendering** — [tests/face_host/](tests/face_host/) compiles the real
  `src/ui/face_runtime.c` + generated tables against small Zephyr shims and
  drives every expression, transition, and micro-reaction through
  `face_runtime_step()`, asserting render-plan validity and that pupil swaps
  always complete (no deadlocks). Run: `bash tests/face_host/run.sh`.

## Android / Gadgetbridge Notes

See [docs/gadgetbridge/README.md](docs/gadgetbridge/README.md) for the current companion protocol and starter scaffold files.

The firmware side already supports:

- custom companion RX/TX characteristics when `CONFIG_KERFUR_ENABLE_COMPANION=y`
- test notifications from shell
- ANCS on iOS
- ANS fallback discovery

## Open Work

The current main gaps are:

- real battery percentage / charger sensing drivers; the behavior and face layers already consume these events, but the repo does not yet include a hardware battery monitor
- deeper power-management entry policy after `APP_EVENT_SLEEP_REQUEST`
- richer Android companion payload parsing beyond the compact category-based protocol
- finishing the Gadgetbridge scaffold's branch-specific notification-enable helper
- moving the remaining hand-maintained face metadata glue into generated output where practical
