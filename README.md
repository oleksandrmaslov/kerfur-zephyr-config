# Kerfur nRF52840 Firmware Base (Zephyr / NCS)

This is a minimal-but-extensible firmware foundation for a "smart pet" keychain.

Current first version goals achieved:
- boots on Zephyr
- initializes SSD1306 display over I2C via shield
- renders an animated alive face
- central event queue
- behavior/state engine with internal variables and utility-based expression selection
- mock event sources (buttons + periodic mock stimuli)
- power/sleep scaffolding
- BLE debug scaffold for event injection

## Assumptions

- Board: `nice_nano` (local board definition in this repo)
- OLED: SSD1306-compatible on I2C address `0x3C`
- Display wiring/config is handled by custom shield:
  - [boards/shields/kerfur_oled/kerfur_oled.overlay](boards/shields/kerfur_oled/kerfur_oled.overlay)
- Board app overlay:
  - [boards/nice_nano.overlay](boards/nice_nano.overlay)
- `sw0` exists for short/long-press test input; `sw1` optional for mock shake.

If your OLED uses another address or needs reset/supply GPIOs, edit the shield overlay first.

## Folder Structure

```text
.
|-- boards/
|   |-- nice_nano.overlay
|   |-- shields/kerfur_oled/...
|   |-- nicekeyboards/nice_nano/...
|   `-- nrf52840dk_nrf52840.overlay
|-- dts/common/nordic/nrf52840_uf2_boot_mode.dtsi
|-- include/
|   |-- app/app.h
|   |-- behavior/
|   |   |-- behavior_engine.h
|   |   `-- pet_state.h
|   |-- ble/ble_manager.h
|   |-- core/
|   |   |-- app_event.h
|   |   `-- event_bus.h
|   |-- drivers/mock_inputs.h
|   |-- power/power_manager.h
|   `-- ui/ui_renderer.h
|-- src/
|   |-- main.c
|   |-- app/app.c
|   |-- behavior/behavior_engine.c
|   |-- ble/ble_manager.c
|   |-- core/event_bus.c
|   |-- drivers/mock_inputs.c
|   |-- power/power_manager.c
|   `-- ui/ui_renderer.c
|-- CMakeLists.txt
|-- Kconfig
`-- prj.conf
```

## Architecture Summary

1. Event Layer (`src/core/event_bus.c`)
- Uses a Zephyr `k_msgq` as central event bus.
- Producers publish typed events with timestamps.
- Consumers process events in main app loop.

2. Internal Pet State (`include/behavior/pet_state.h`)
- Core variables: `energy`, `sleepiness`, `attachment`, `boredom`, `stress`, `arousal`, `social_load`
- Includes `last_interaction_timestamp_ms`, `current_mode`, `expression`, BLE and battery flags.

3. Behavior Engine (`src/behavior/behavior_engine.c`)
- Consumes events and applies state deltas.
- Applies time evolution on `APP_EVENT_TICK_1S`.
- Computes expression via utility scores, not direct hardcoded event->emotion mapping.
- Supports modes: `AWAKE`, `DROWSY`, `ASLEEP`.

4. UI Renderer (`src/ui/ui_renderer.c`)
- Separate from behavior logic.
- Uses LVGL to render and animate the face.
- Keeps display access via Zephyr display API (blank on/off).
- Uses periodic blink and mouth movement for "alive" effect.
- Includes anti-burn-in pixel shift foundation.

5. Mock Inputs (`src/drivers/mock_inputs.c`)
- `sw0` short press: `USER_BUTTON_PRESS` + `MOCK_PET`
- `sw0` long press: `USER_BUTTON_PRESS` + `MOCK_NOTIFICATION`
- `sw1` press (if available): `MOCK_SHAKE`
- 1 second timer emits `TICK_1S`
- Optional periodic mock events (Kconfig controlled)

6. Power Scaffolding (`src/power/power_manager.c`)
- Tracks real interaction timeout separately from ambient events.
- Emits `IDLE_TIMEOUT` and `SLEEP_REQUEST`.
- Controls display blanking state (UI uses it now).
- Future extension point for deep sleep entry/wake source policy.

7. BLE Scaffold (`src/ble/ble_manager.c`)
- Minimal connectable advertiser.
- Custom write characteristic for event injection.
- Publishes `BLE_CONNECTED`/`BLE_DISCONNECTED`.
- Write byte command mapping:
  - `1`: `MOCK_PET`
  - `2`: `MOCK_SHAKE`
  - `3`: `MOCK_NOTIFICATION`
  - `4`: `SLEEP_REQUEST`
  - `5`: `WAKE`

## Build / Flash

From Zephyr workspace shell (`nice!nano` + shield):

```bash
west build -p always -b nice_nano -d build/nice_nano_local -- -DSHIELD=kerfur_oled
west flash
```

UF2 output is generated at:

```text
build/nice_nano_local/zephyr/zephyr.uf2
```

Monitor logs:

```bash
west build -t run
```

## What To Edit First For Your Hardware

1. Display wiring and bus:
- [boards/shields/kerfur_oled/kerfur_oled.overlay](boards/shields/kerfur_oled/kerfur_oled.overlay)
- Change I2C address and SSD1306 parameters if needed.

2. Input mapping:
- [src/drivers/mock_inputs.c](src/drivers/mock_inputs.c)
- Replace `sw0/sw1` with your actual input GPIOs or touch driver events.

3. Behavior tuning:
- [src/behavior/behavior_engine.c](src/behavior/behavior_engine.c)
- Tune decay/growth constants and utility score functions.

4. Timeouts and cadence:
- [Kconfig](Kconfig)
- Set idle/sleep/UI-frame defaults.

## TODO (Next Steps)

- Add battery monitor driver and publish `BATTERY_LOW`/recovery events.
- Add real touch input module and distinguish pet quality/intensity.
- Add IMU integration for real shake/motion events.
- Add day/night schedule source and circadian behavior modulation.
- Extend BLE service with readable debug state characteristic.
- Add persistence for pet state snapshot.
- Add deeper power states (`pm_state_force`) and wake source policy.
- Add OLED dimming and full burn-in policy (shift + dim + blank strategy).
