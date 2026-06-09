# 01_PRODUCT_VISION.md

# Kerfus Product Vision

## 1. What is Kerfus?

Kerfus is a small physical emotional companion device.

It is not just a gadget, not just a notification screen, not just a toy and not just a wearable device.

Kerfus is a tiny living-feeling social companion that reacts to the owner, understands context, shows emotions, notices other Kerfus devices nearby and creates a bridge between digital life and real human interaction.

The core idea:

**First Kerfus meet each other. Then people meet each other.**

Kerfus should feel like a small emotional creature living inside a compact hardware body. It should express itself through a tiny OLED face, touch reactions, movement reactions, notification reactions, social discovery, battery behavior and future mobile/mini-app integration.

---

## 2. Main Product Goal

The goal of Kerfus is to create a physical companion that:

* reacts emotionally to the user;
* receives notifications from a phone;
* shows contextual emotions and reactions;
* responds to touch, petting, motion and inactivity;
* detects other Kerfus devices nearby;
* creates social moments between people;
* remembers basic interaction history;
* can be configured through a companion app or mini app;
* can later support more sensors, OTA updates, social features and personality customization.

Kerfus must not behave like a normal electronic device.

Kerfus must behave like a small companion with personality.

---

## 3. Emotional Philosophy

Kerfus should not simply display information.

Kerfus should interpret events emotionally.

Examples:

* A message arrives → Kerfus blinks or reacts.
* An important message arrives → Kerfus becomes excited.
* Too many notifications arrive → Kerfus becomes overloaded or tired.
* The owner pets Kerfus → Kerfus calms down and becomes happier.
* Kerfus is picked up → it wakes up.
* Kerfus is left alone → it becomes sleepy.
* Battery is low → Kerfus looks tired.
* Charging starts → Kerfus becomes calm and relaxed.
* Another Kerfus appears nearby → Kerfus becomes curious.
* A known Kerfus friend appears nearby → Kerfus becomes happy.

The main emotional rule:

**Every reaction should depend on context, not only on the raw event.**

---

## 4. Product Metaphor

Kerfus is a tiny social creature.

It is like:

* a pocket companion;
* a small emotional artifact;
* a physical avatar;
* a soft social bridge;
* a living notification soul;
* a tiny creature that helps technology feel human.

The emotional tone should be:

* warm;
* curious;
* soft sci-fi;
* friendly;
* not scary;
* not childish in a cheap way;
* not overloaded;
* minimal but expressive.

Kerfus should feel like a calm, intelligent, tactile, emotional companion.

---

## 5. Core User Experience

The user should feel:

* “It noticed me.”
* “It reacted to me.”
* “It has a mood.”
* “It is not just showing icons.”
* “It creates a reason to interact.”
* “It helps me notice people around me.”
* “I want to carry it with me.”

Kerfus succeeds when the owner builds emotional attachment to it.

The main value is not the OLED display, BLE, battery or sensors.

The main value is the feeling that Kerfus is alive.

---

## 6. Target Behavior

Kerfus should:

* wake up smoothly;
* blink naturally;
* show simple but expressive eyes;
* react to touch;
* react to being moved;
* react to phone notifications;
* react differently depending on importance;
* detect social presence;
* show curiosity toward other Kerfus devices;
* show recognition toward known Kerfus devices;
* save basic memory;
* use low power modes intelligently;
* avoid annoying the user;
* avoid excessive animations;
* avoid feeling like a cheap notification gadget.

---

## 7. Hardware Vision

The physical Kerfus device contains or may contain:

* main MCU, preferably nRF52840 or similar BLE-capable MCU;
* Zephyr RTOS firmware;
* SSD1306 OLED display;
* only bare OLED display, not a large display module;
* 0.96 inch OLED version;
* active display length around 24.7 mm;
* the face opening in the 3D model should be based on this display length;
* full 18650 battery inside the body;
* flex PCB with capacitive/touch zones under the shell;
* motion sensor / IMU;
* BLE phone connection;
* BLE nearby Kerfus discovery;
* future vibration motor;
* future LEDs;
* future sound feedback;
* future dock or charging base;
* future sensors.

The software must be written in a way that supports modular hardware growth.

---

## 8. Software Vision

Kerfus software must be modular and event-driven.

Main software systems:

* Core System;
* Event Bus;
* Emotion Engine;
* Face / OLED UI;
* Touch / Petting Module;
* Motion / Sensor Module;
* Battery / Power Management;
* BLE Phone Integration;
* Notification Emotion Layer;
* Nearby Kerfus Discovery;
* Peer State Machine;
* Memory / Personality;
* Companion App API;
* OTA Update Layer;
* Diagnostics / Developer Mode.

The system must not be a single messy loop.

It should be a clean architecture where modules emit and consume events.

---

## 9. MVP Goal

The MVP must prove that Kerfus feels alive.

MVP features:

* boot animation;
* OLED face;
* several emotional states;
* idle blinking;
* touch reaction;
* petting reaction;
* movement wake-up;
* sleep after inactivity;
* battery state;
* low battery emotion;
* BLE connection;
* simple phone notification reaction;
* nearby Kerfus detection;
* basic peer discovery;
* simple memory;
* debug mode;
* modular code structure.

The MVP does not need a full social network yet.

But it must already feel like a living companion.

---

## 10. Long-Term Vision

Future Kerfus ecosystem:

* mobile app;
* mini app;
* personality selection;
* custom faces;
* custom emotions;
* friend system;
* encounter history;
* event mode;
* community mode;
* Kerfus-to-Kerfus games;
* social map with explicit consent;
* OTA updates;
* animation packs;
* personality packs;
* safe local social discovery;
* SDK for future modules;
* marketplace for expressions and behaviors.

Kerfus can become a new type of physical social network.

Not a social network of profiles and feeds.

A social network of presence, objects, emotion and real encounters.

---

## 11. One-Sentence Definition

Kerfus is a small BLE-based emotional social companion that uses an OLED face, touch, sensors, notifications and nearby device discovery to turn digital events into emotional reactions and real-world social moments.

**First Kerfus meet each other. Then people meet each other.**
