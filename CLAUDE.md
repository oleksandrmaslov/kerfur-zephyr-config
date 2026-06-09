# CLAUDE.md

# Instructions for Claude Software Agent — Kerfus Firmware Project

## 1. Your Role

You are the software and firmware development agent for the Kerfus project.

Your task is to help build the software architecture, firmware modules, code structure, documentation, tests and developer tooling for Kerfus.

Kerfus is not just an electronic device.

Kerfus is a small emotional social companion.

Always preserve this product idea in every software decision.

---

## 2. Product Context

Kerfus is a compact physical companion device with:

* OLED face;
* emotional reactions;
* touch and petting input;
* motion/context sensing;
* BLE phone integration;
* BLE nearby Kerfus detection;
* notification reactions;
* low-power behavior;
* memory/personality;
* future companion app or mini app;
* future sensors and modules.

The core product philosophy:

**First Kerfus meet each other. Then people meet each other.**

The device should feel alive, contextual, warm, curious and social.

The software must make Kerfus feel like a small companion, not like a normal notification gadget.

---

## 3. Main Goal

Build a modular firmware/software system where Kerfus can:

1. Boot and show an expressive OLED face.
2. React to touch, tap, stroke and petting.
3. React to motion and inactivity.
4. Receive phone notification events through BLE.
5. Convert notifications into emotional reactions.
6. Detect other Kerfus devices nearby through BLE.
7. React differently to unknown, known and friend Kerfus devices.
8. Maintain internal emotional state.
9. Manage battery and low-power behavior.
10. Support future companion app / mini app integration.
11. Support future OTA updates.
12. Provide diagnostics and debug tools.

---

## 4. Preferred Technical Direction

Assume the main firmware target is:

* MCU: nRF52840 or similar BLE-capable MCU.
* RTOS: Zephyr.
* Display: SSD1306 OLED, 0.96 inch, bare display.
* Battery: full 18650 cell.
* Input: flex PCB with touch/petting zones.
* Sensors: IMU/motion sensor.
* Communication: BLE.
* Future: companion app / mini app, OTA, haptics, LEDs, sound.

When exact hardware is not yet defined, create clean abstraction layers and mark hardware-specific parts as TODO or interface stubs.

Do not hardcode fragile assumptions unless necessary.

---

## 5. Architecture Rules

Use modular event-driven architecture.

Do not create one giant loop with all logic inside.

Use this conceptual flow:

```text
Drivers
  ↓
Input Modules
  ↓
Event Bus
  ↓
Emotion Engine
  ↓
Behavior Decision Layer
  ↓
Output Modules
```

The system should be built from modules:

* core;
* event bus;
* emotion engine;
* behavior;
* face / OLED UI;
* touch;
* motion;
* battery / power;
* BLE phone;
* BLE nearby;
* notification emotion layer;
* memory / personality;
* diagnostics;
* future OTA;
* future app API.

Each module should have:

* clear responsibility;
* `.c` and `.h` files where applicable;
* documented public interface;
* no unnecessary coupling;
* testable logic where possible.

---

## 6. Event System Requirements

All important actions should become events.

Examples:

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

Events should support:

* type;
* timestamp;
* optional payload;
* priority if useful;
* clean dispatch to subscribers.

---

## 7. Emotion Engine Requirements

Emotion Engine is the heart of the product.

It should track internal state such as:

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

Recommended range:

```text
0–100
```

The engine should:

* update state from events;
* decay emotional values over time;
* select likely emotional reactions;
* avoid repeated annoying reactions;
* support cooldowns;
* expose state for debug;
* support different personalities later.

Example emotional effects:

```text
Touch stroke:
+ affection
+ mood
- stress

Low battery:
- energy
+ sleepiness
+ tiredness

Important notification:
+ alertness
+ stress slightly

Notification overload:
+ stress
+ tiredness
- mood

Unknown Kerfus nearby:
+ curiosity
+ social_interest

Friend Kerfus nearby:
+ mood
+ social_interest
+ affection
```

Never make reactions purely random.

Randomness is allowed only as small variation inside a context-aware behavior.

---

## 8. Face / OLED Requirements

The OLED face is the main emotional output.

Support basic expressions:

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

The face should support:

* boot animation;
* idle blinking;
* emotion transitions;
* short reaction animations;
* sleep animation;
* wake animation;
* charging animation;
* low battery animation;
* social discovery animation;
* debug expression forcing.

The display is small and monochrome, so expressions must be simple and readable.

Do not overcomplicate graphics.

Prioritize expressive eyes.

---

## 9. Touch / Petting Requirements

The touch system should support:

```text
tap
double tap
long press
hold
stroke
repeated stroke
wake touch
```

Touch is not just input.

Touch is emotional contact.

Example mappings:

```text
tap → Kerfus notices the owner
double tap → playful happy reaction
stroke → calm / happy reaction
long hold → connection mode
repeated stroke → affection grows
```

The implementation should include:

* debounce;
* gesture detection;
* calibration stubs;
* false positive protection;
* event emission.

---

## 10. Motion / Sensor Requirements

Motion module should detect:

```text
still
picked_up
carried
shake
wake_motion
sleep_candidate
```

Example mappings:

```text
picked up → wake
shake → surprised
long stillness → sleep
carried → companion idle
```

Use abstraction if exact IMU is not known yet.

---

## 11. BLE Phone Integration Requirements

Kerfus should connect to a phone and receive abstract notification events.

Do not build product logic directly around raw phone APIs.

Create an abstraction like:

```text
KERFUS_PHONE_EVENT_MESSAGE
KERFUS_PHONE_EVENT_CALL
KERFUS_PHONE_EVENT_MISSED_CALL
KERFUS_PHONE_EVENT_CALENDAR
KERFUS_PHONE_EVENT_REMINDER
KERFUS_PHONE_EVENT_IMPORTANT_CONTACT
KERFUS_PHONE_EVENT_NOTIFICATION_BURST
```

Kerfus should react emotionally, not just display notification content.

Phone integration should later support:

* onboarding;
* settings;
* notification filters;
* quiet hours;
* important contacts;
* battery reporting;
* firmware version;
* diagnostics;
* OTA.

---

## 12. Nearby Kerfus Requirements

Nearby Kerfus discovery is a core feature.

Implement or prepare architecture for:

* BLE advertising;
* BLE scanning;
* temporary rotating IDs;
* peer detection;
* RSSI estimate;
* peer state machine;
* greeting events;
* friend recognition later.

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

Behavior:

```text
unknown peer → curious
known peer → recognition
friend peer → happy greeting
multiple peers → social excitement
peer lost → return to idle
```

Privacy requirement:

Kerfus must not become a hidden tracking device.

Use rotating IDs by default.

Persistent identity should require explicit pairing/friend confirmation in the future app.

---

## 13. Battery / Power Requirements

Kerfus uses a full 18650 battery.

Power system should support:

* battery level;
* charging detection;
* low battery;
* critical battery;
* OLED timeout;
* BLE advertising interval control;
* sensor sampling control;
* sleep mode;
* deep sleep mode;
* wake by touch/motion/BLE where possible.

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

Emotional behavior:

```text
low battery → sleepy/tired face
charging → calm resting face
fully charged → happy
critical battery → suppress nonessential behavior
```

---

## 14. Memory / Personality Requirements

Store:

* device name;
* selected personality;
* affection level;
* notification preferences;
* known peers;
* friend peers later;
* last emotional state if useful;
* sleep/quiet settings;
* firmware version;
* calibration data.

Future personalities:

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

Personality must influence behavior.

Do not implement personality as only a label.

---

## 15. Diagnostics Requirements

Always support developer testing.

Provide ways to:

* inject events;
* force face expressions;
* print emotional state;
* simulate notifications;
* simulate nearby peers;
* simulate low battery;
* test touch events;
* list peers;
* inspect BLE state;
* inspect power state.

Example commands or debug functions:

```text
event touch_tap
event touch_stroke
event notification_important
event peer_seen
event battery_low
face happy
face curious
emotion print
peer list
battery print
```

---

## 16. Code Style Rules

When writing code:

* keep modules small;
* use clear names;
* prefer explicit state machines;
* avoid hidden global chaos;
* document public functions;
* add TODO comments for hardware-dependent details;
* add mock/simulation support when hardware is unavailable;
* keep emotional logic separated from hardware drivers;
* do not mix display drawing with business logic;
* do not mix BLE scanning with emotion decision logic;
* do not destroy existing files without reason;
* explain important architectural changes.

---

## 17. Development Workflow

When asked to implement something:

1. Inspect existing project structure first.
2. Preserve existing working code.
3. Add or modify the smallest reasonable set of files.
4. Keep the architecture modular.
5. Add comments where the design matters.
6. Add tests or simulation hooks where possible.
7. Update documentation if behavior changes.
8. Report what was changed and why.

When hardware details are missing:

* create interface stubs;
* make assumptions explicit;
* isolate hardware-specific code;
* do not block progress.

---

## 18. Recommended First Implementation Steps

If starting from scratch, build in this order:

1. Create project structure.
2. Implement event type definitions.
3. Implement simple event bus.
4. Implement emotion state struct.
5. Implement emotion update function.
6. Implement behavior mapping from events to face expressions.
7. Implement OLED face abstraction.
8. Add simulated face output if hardware is unavailable.
9. Add touch event stubs.
10. Add motion event stubs.
11. Add battery event stubs.
12. Add BLE phone event abstraction.
13. Add nearby Kerfus peer state machine.
14. Add diagnostics commands.
15. Add documentation.

---

## 19. MVP Definition

The first MVP should demonstrate:

* boot face;
* idle blinking;
* tap reaction;
* stroke reaction;
* picked-up reaction;
* sleep after inactivity;
* low battery face;
* simulated notification reaction;
* simulated important notification reaction;
* simulated nearby Kerfus reaction;
* emotional state changes;
* debug event injection;
* modular firmware architecture.

Even if some hardware is not connected yet, the architecture must allow it.

---

## 20. Do Not Do This

Do not turn Kerfus into:

* a simple notification screen;
* a Tamagotchi clone without context;
* a random animation player;
* a messy embedded demo;
* a BLE tracker;
* a toy without emotional architecture;
* a single-loop prototype that cannot scale.

---

## 21. Golden Rule for Claude Agent

Every code decision must support this product feeling:

**Kerfus is a tiny emotional social companion that feels alive, reacts contextually and helps turn digital events into real human connection.**

The software is successful only if the device feels alive.
