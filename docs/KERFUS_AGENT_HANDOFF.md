# Kerfus / Kerfur — Agent Handoff

> For the next Claude / Codex / ChatGPT session. Read this together with
> `KERFUS_PROJECT_STATE.md` (detailed state) and `KERFUS_NEXT_STEPS.md` (roadmap).
> Last updated: 2026-06-09.

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

## 5. What changed in this session

- Audited the repo; ran toolchain/build diagnosis.
- Created `docs/KERFUS_PROJECT_STATE.md`, `docs/KERFUS_NEXT_STEPS.md`, and this
  handoff. **No source code was modified.** No files deleted.
- Ran a pristine verification build into `build/audit_nice_nano`: **SUCCESS**
  (SDK 1.0.1, `nice_nano/nrf52840`, FLASH 72.3% / RAM 31.5%). Details in
  PROJECT_STATE §6.

## 6. What must NOT be broken

- The event-bus contract in `include/core/app_event.h` (everything depends on it;
  note the backward-compat alias `#define`s at the bottom).
- The face codegen flow: `assets/face/kerfur_faces.json` → `tools/face_codegen.py`
  → checked-in `*/generated/*` — regenerate, don't hand-edit generated files.
- Hardware-graceful degradation: firmware must still build/run if `motion0` or
  `touch0` aliases are absent.
- Privacy rule for nearby: ephemeral rotating IDs by default; persistent identity
  only via explicit future-app pairing. Never turn it into a tracker.
- Keep emotional logic out of drivers and BLE out of emotion decisions
  (`CLAUDE.md` §16).

## 7. What the next agent should do first

1. Read `CLAUDE.md`, then `KERFUS_PROJECT_STATE.md`.
2. Resolve open questions in §8 with the owner.
3. Do the P0 stabilization (NEXT_STEPS I1–I3): fill/remove empty docs, pin SDK,
   confirm clean build.
4. Then the two real "living device" gaps: battery driver (M1) and true sleep
   (M2). Add behavior/nearby unit tests (M3) before larger refactors.

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
