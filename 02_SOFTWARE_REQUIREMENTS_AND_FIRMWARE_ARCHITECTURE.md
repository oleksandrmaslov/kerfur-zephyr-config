# 02_SOFTWARE_REQUIREMENTS_AND_FIRMWARE_ARCHITECTURE.md

# Kerfus Software Requirements and Firmware Architecture

## 1. Software Goal

The Kerfus software must make the device feel alive.

The software must not only operate hardware.

It must create a coherent emotional behavior system.

Every module should support the main product goal:

**Kerfus is a small contextual emotional companion that reacts to the owner, notifications, physical interaction, sensors and nearby Kerfus devices.**

---

## 2. Preferred Technical Stack

Recommended base:

* MCU: nRF52840 or compatible BLE-capable MCU.
* RTOS: Zephyr.
* Language: C for firmware core.
* Optional tools: Python scripts for asset conversion, animation tooling, tests and build utilities.
* Display: SSD1306 OLED.
* Communication: BLE.
* Future: companion app / mini app.

The codebase must be structured so hardware can change without rewriting product logic.

---

## 3. Architecture Principle

The firmware must be modular and event-driven.

Do not build Kerfus as one giant loop.

Use this conceptual architecture:

```text
Hardware Drivers
    ↓
Input Modules
    ↓
Event Bus
    ↓
Context / State / Emotion Engine
    ↓
Behavior Decision Layer
    ↓
Output Modules
    ↓
OLED Face / BLE / Haptics / Logs / App API
```

Each module should have a clear responsibility.

Modules should communicate through events where possible.

---

## 4. Core Modules

## 4.1 Core System

Responsibilities:

* boot sequence;
* hardware initialization;
* module initialization;
* main runtime loop;
* system state;
* error handling;
* timing;
* sleep/wake coordination;
* debug mode;
* watchdog support;
* safe recovery.

---

## 4.2 Event Bus

The Event Bus is the nervous system of Kerfus.

All important things should become events.

Example events:

```c
KERFUS_EVENT_BOOT_COMPLETE
KERFUS_EVENT_TOUCH_TAP
KERFUS_EVENT_TOUCH_DOUBLE_TAP
KERFUS_EVENT_TOUCH_LONG_PRESS
KERFUS_EVENT_TOUCH_STROKE
KERFUS_EVENT_MOTION_PICKED_UP
KERFUS_EVENT_MOTION_SHAKE
KERFUS_EVENT_MOTION_STILL
KERFUS_EVENT_NOTIFICATION_RECEIVED
KERFUS_EVENT_NOTIFICATION_IMPORTANT
KERFUS_EVENT_NOTIFICATION_OVERLOAD
KERFUS_EVENT_BATTERY_LOW
KERFUS_EVENT_BATTERY_CRITICAL
KERFUS_EVENT_CHARGING_STARTED
KERFUS_EVENT_CHARGING_STOPPED
KERFUS_EVENT_PEER_SEEN
KERFUS_EVENT_PEER_NEAR
KERFUS_EVENT_PEER_FRIEND_SEEN
KERFUS_EVENT_SLEEP_ENTER
KERFUS_EVENT_SLEEP_EXIT
```

The Event Bus should allow:

* publishing events;
* subscribing modules;
* priority handling;
* event timestamps;
* optional payloads.

---

## 4.3 Emotion Engine

Emotion Engine is the core of the Kerfus personality.

It receives events and updates internal emotional variables.

Suggested emotional variables:

```text
energy
mood
curiosity
affection
social_interest
alertness
tiredness
stress
sleepiness
```

Each variable can be represented as a small integer or fixed-point value.

Example range:

```text
0–100
```

Emotion Engine must:

* update emotional state based on events;
* decay values over time;
* choose reactions depending on state;
* avoid repeating the same reaction too often;
* respect cooldowns;
* react differently based on context;
* expose current state to debug tools;
* trigger face animations.

Example:

```text
Touch stroke:
+ affection
+ mood
- stress

Many notifications:
+ stress
- mood
+ tiredness

Low battery:
- energy
+ sleepiness

Nearby unknown Kerfus:
+ curiosity
+ social_interest

Nearby friend Kerfus:
+ mood
+ social_interest
+ affection
```

---

## 4.4 Behavior Decision Layer

This layer decides what Kerfus actually does.

It receives:

* current emotional state;
* current event;
* battery status;
* user interaction status;
* social context;
* notification context;
* cooldowns;
* priority.

It outputs:

* face animation;
* BLE response;
* vibration pattern in the future;
* app event;
* sound/LED reaction in the future.

Example logic:

```text
If event = notification and stress is high:
    show overload animation

If event = touch stroke and battery is not critical:
    show happy/calm animation

If event = peer seen and peer is unknown:
    show curious animation

If event = peer seen and peer is friend:
    show happy greeting animation

If battery is critical:
    suppress nonessential animations
    show sleepy low-battery face
```

---

## 4.5 Face / OLED UI Module

The Face Module renders Kerfus emotions on SSD1306 OLED.

Responsibilities:

* initialize SSD1306;
* draw eyes/face;
* support animation frames;
* support idle blinking;
* support transitions;
* show boot animation;
* show sleep animation;
* show notification reactions;
* show social reactions;
* show low battery;
* show charging;
* support debug face.

Basic expressions:

```text
neutral
happy
calm
curious
surprised
sleepy
tired
shy
excited
sad
overloaded
charging
low_battery
social_scan
greeting
```

Design rule:

The face should be minimal, readable and emotionally expressive on a tiny monochrome OLED.

---

## 4.6 Touch / Petting Module

This module handles the flex PCB touch zones.

Supported interactions:

```text
tap
double tap
long press
hold
stroke
repeated stroke
wake touch
```

Responsibilities:

* read capacitive/touch input;
* debounce;
* detect gestures;
* emit touch events;
* classify petting gestures;
* support calibration;
* avoid false positives.

Touch must feel emotional, not like a button press.

Example:

```text
tap → Kerfus looks at user
double tap → Kerfus becomes happy
stroke → Kerfus calms down
long hold → connection mode
repeated stroke → affection increases
```

---

## 4.7 Motion / Sensor Module

This module handles IMU/motion detection.

Supported states:

```text
still
picked_up
carried
shake
pocket_motion
sleep_candidate
wake_motion
```

Responsibilities:

* initialize IMU;
* detect movement patterns;
* emit motion events;
* wake system on movement;
* help power management;
* avoid excessive sensor polling.

Example:

```text
picked up → wake animation
shake → surprised reaction
long stillness → sleep candidate
carried → companion idle
```

---

## 4.8 Battery / Power Management Module

Kerfus uses a full 18650 battery.

Power Management must support:

* battery level reading;
* charging detection;
* low battery;
* critical battery;
* OLED timeout;
* BLE interval adjustment;
* sensor sampling adjustment;
* sleep mode;
* deep sleep mode;
* wake sources.

Power modes:

```text
ACTIVE
COMPANION_IDLE
SOCIAL_SCAN
SLEEP
DEEP_SLEEP
CHARGING
LOW_BATTERY
CRITICAL_BATTERY
```

Emotional battery behavior:

* low battery → sleepy/tired face;
* charging → calm/resting animation;
* fully charged → happy wake;
* critical → suppress social and notification reactions.

---

## 4.9 BLE Phone Integration Module

This module connects Kerfus to a phone.

Responsibilities:

* BLE advertising;
* BLE connection;
* pairing;
* receive basic notification events;
* sync time;
* send device state to app;
* receive settings from app;
* expose diagnostics;
* support future OTA.

Phone events should be abstracted.

Kerfus should not depend directly on app-specific logic.

Example phone event types:

```text
message
call
missed_call
calendar
reminder
system
important_contact
silent_notification
notification_burst
```

---

## 4.10 Notification Emotion Layer

This layer turns phone events into Kerfus behavior.

Rules:

* do not show every notification equally;
* classify importance;
* avoid annoying the user;
* support quiet hours;
* support important contacts;
* detect notification overload;
* map notification category to emotional reaction.

Examples:

```text
important message → excited / attentive
normal message → blink
missed call → concerned
many notifications → overloaded
calendar reminder → calm attention
night notification → minimal reaction
```

---

## 4.11 Nearby Kerfus Social Module

This module detects other Kerfus devices nearby.

Responsibilities:

* BLE advertising;
* BLE scanning;
* rotating temporary IDs;
* peer detection;
* RSSI-based proximity estimate;
* peer state machine;
* social greeting;
* cooldowns;
* privacy protection.

Peer states:

```text
NONE
SEEN
NEAR
INTERACTING
KNOWN
FRIEND
COOLDOWN
LOST
```

Possible peer events:

```text
KERFUS_EVENT_PEER_SEEN
KERFUS_EVENT_PEER_NEAR
KERFUS_EVENT_PEER_LOST
KERFUS_EVENT_PEER_GREETING_RECEIVED
KERFUS_EVENT_PEER_GREETING_ACK
KERFUS_EVENT_PEER_FRIEND_SEEN
```

Social behavior:

* unknown Kerfus nearby → curiosity;
* repeated encounter → recognition;
* friend nearby → warm greeting;
* multiple Kerfus nearby → social excitement;
* no interaction after greeting → cooldown.

Privacy rule:

Kerfus must not become a tracker.

Use rotating IDs where possible.

Permanent identity should only be used with explicit pairing or friendship confirmation.

---

## 4.12 Memory / Personality Module

Kerfus should remember enough to feel consistent.

Stored data:

* device name;
* selected personality;
* owner settings;
* affection level;
* mood baseline;
* known Kerfus list;
* friend Kerfus list;
* notification preferences;
* interaction statistics;
* last seen peers;
* sleep schedule;
* animation preferences.

Possible personalities:

```text
curious
shy
calm
playful
sleepy
brave
social
protective
```

Personality should change reaction style.

Example:

```text
Curious Kerfus reacts more strongly to unknown peers.
Shy Kerfus reacts softly to unknown peers.
Playful Kerfus reacts more to touch.
Sleepy Kerfus uses slower animations.
```

---

## 4.13 Companion App / Mini App API

The firmware should expose an API for a future app.

App functions:

* onboarding;
* rename Kerfus;
* choose personality;
* configure notification filters;
* configure quiet hours;
* see battery level;
* see firmware version;
* see basic mood state;
* see known Kerfus;
* confirm friendship;
* update firmware;
* run diagnostics;
* test animations;
* change face style;
* export logs.

---

## 4.14 OTA Update Module

Future OTA support should include:

* firmware versioning;
* safe update process;
* rollback/fallback;
* update progress;
* error recovery;
* BLE DFU if supported;
* app-triggered update.

---

## 4.15 Diagnostics / Developer Mode

Developer Mode is required.

It should allow:

* injecting fake events;
* simulating touch;
* simulating motion;
* simulating notifications;
* simulating nearby Kerfus;
* testing face animations;
* reading emotional state;
* reading battery state;
* reading BLE status;
* exporting logs;
* verifying event bus behavior.

Example debug commands:

```text
event touch_tap
event touch_stroke
event battery_low
event notification_important
event peer_seen
face happy
face curious
emotion print
battery print
peer list
```

---

## 5. Recommended Folder Structure

Suggested project structure:

```text
kerfus-firmware/
  app/
    main.c
    app_config.h

  modules/
    core/
      kerfus_core.c
      kerfus_core.h

    events/
      kerfus_event_bus.c
      kerfus_event_bus.h
      kerfus_events.h

    emotion/
      kerfus_emotion.c
      kerfus_emotion.h
      kerfus_emotion_types.h

    behavior/
      kerfus_behavior.c
      kerfus_behavior.h

    face/
      kerfus_face.c
      kerfus_face.h
      kerfus_face_assets.h
      animations/

    touch/
      kerfus_touch.c
      kerfus_touch.h

    motion/
      kerfus_motion.c
      kerfus_motion.h

    battery/
      kerfus_battery.c
      kerfus_battery.h
      kerfus_power.c
      kerfus_power.h

    ble/
      kerfus_ble.c
      kerfus_ble.h
      kerfus_phone.c
      kerfus_phone.h
      kerfus_nearby.c
      kerfus_nearby.h

    memory/
      kerfus_memory.c
      kerfus_memory.h

    diagnostics/
      kerfus_debug.c
      kerfus_debug.h

  drivers/
    display/
    touch/
    imu/
    battery/

  docs/
    01_PRODUCT_VISION.md
    02_SOFTWARE_REQUIREMENTS_AND_FIRMWARE_ARCHITECTURE.md
    CLAUDE.md

  tools/
    animation_converter/
    asset_builder/

  tests/
    emotion_tests/
    event_bus_tests/
    behavior_tests/
```

---

## 6. Development Priorities

Build in this order:

1. Project skeleton.
2. Event Bus.
3. OLED face basic rendering.
4. Emotion Engine basic state.
5. Behavior mapping.
6. Touch simulation.
7. Real touch input.
8. Motion wake/sleep.
9. Battery state.
10. BLE basic connection.
11. Notification event simulation.
12. Nearby Kerfus simulation.
13. Nearby BLE discovery.
14. Memory.
15. App API.
16. OTA.

---

## 7. MVP Acceptance Criteria

MVP is acceptable when:

* Kerfus boots and shows a face.
* Kerfus blinks in idle mode.
* Kerfus reacts to tap.
* Kerfus reacts to petting/stroke.
* Kerfus wakes on movement.
* Kerfus sleeps after inactivity.
* Kerfus shows low battery emotion.
* Kerfus can receive a simulated notification.
* Kerfus can show a different reaction for important notification.
* Kerfus can detect or simulate another Kerfus nearby.
* Kerfus has event-based architecture.
* Kerfus has readable modular code.
* Developer can inject debug events.
* Code is ready for future app and OTA integration.

---

## 8. Golden Rule

Do not build Kerfus as a gadget.

Build Kerfus as a small emotional being implemented through firmware.

Every technical decision should support this feeling:

**Kerfus is alive, contextual, social and emotionally present.**
