# Kerfus Emotion Runtime

> How the emotional core works after the 2026-06-11/12 reworks.
> Code: `src/behavior/behavior_engine.c`, `src/behavior/appraisal.c`,
> `src/behavior/emotion_memory.c`, `src/behavior/micro_reaction.c`,
> `src/ui/face_runtime.c`; carry context from
> `src/drivers/in_hand_detector.c` + `src/drivers/motion_classifier.c`.
> Product law: `CLAUDE.md` — "Kerfus is a tiny emotional social companion that
> feels alive, reacts contextually and helps turn digital events into real
> human connection."

---

## 1. Emotional layers (fastest → slowest)

| Layer | Lives in | Timescale | Purpose |
|-------|----------|-----------|---------|
| Micro-reactions | `micro_reaction.c` | 0.2–2 s | Immediate visible acknowledgement (blink, startle, bounce, bow…), with per-reaction cooldowns |
| Afterglow context | `behavior_engine.c` runtime (`stimulation`, `comfort`, `social_warmth`) | ~1 min | Emotional momentum: what just happened colors what happens next |
| Drives | `pet_state` (energy, sleepiness, attachment, boredom, stress, arousal, social_load, trust, curiosity) | minutes | The classic needs/arousal model; decays via 1 s / 10 s / 60 s / 5 min / 30 min ticks |
| Mood | `pet_state.mood` (0–100, 50 = neutral) | hours | Day-scale valence/temperament: how life has felt lately |
| Persisted traits | settings/NVS via `emotion_memory.c` | across reboots | Attachment, trust, mood, lifetime petting count, personality, worn style — "it is still the same pet" |
| Intent | runtime (`REST/SEEK_ATTENTION/OBSERVE/PLAY/SELF_SOOTHE/WITHDRAW`) | ~5 s resolution, hysteresis | A behavioral "leaning" that biases expression choice and idle behavior |

Orthogonal to the timescales, a deterministic **situation layer** (§1a)
decides *where the pet is living right now* and selects how the emotional
state is allowed to show.

Doc-vocabulary mapping (CLAUDE.md §7 ↔ code): mood↔mood, affection↔attachment,
social_interest↔social_load/social_warmth, alertness↔arousal,
tiredness/sleepiness↔sleepiness, plus boredom/trust/curiosity as extensions.

## 1a. Situation layer + normalized appraisal (production scoring)

The old per-expression formulas mixed incommensurate scales (SLEEPY could
reach ~138 while CALM topped out near 85) and buried ~60 magic numbers in a
switch. They were replaced by a hybrid **context + table** design:

1. **Situation resolver** (`resolve_situation`, deterministic priority
   ladder): CHARGING > SOCIAL (peer present) > ENGAGED (in hand) >
   WORN_QUIET / WORN_LIVELY (carry context WORN, by worn style) > RESTING.
2. **Feature vector** (`appraisal_compute_features`): every input is
   normalized to 0..100 — drives pass through, recencies become linear
   ramps (fresh-pet 8 s, neglect 2→30 min, …), booleans become 0/100.
3. **Weight table** (`g_appraisal[expr]` in `appraisal.c`):
   `score = bias + Σ(weight × feature)/100`, every row calibrated to the
   same envelope (typical winning scores 40..90, ceiling ~130). All tuning
   lives in this one table.
4. **Situation policy** (`g_situations[]`): per-situation additive bias
   (e.g. ENGAGED leans HAPPY/CURIOUS and forbids LONELY from winning —
   being held *is* attention) and an allowed-expression mask
   (WORN_QUIET only shows CALM/CONTENT/CURIOUS/SLEEPY/DRAINED/ASLEEP).
   CALM is always allowed (guaranteed fallback).
5. The engine keeps **intent alignment, transition affinity and
   hysteresis** on top, and the petting/asleep/forced overrides still win.

`appraisal.c` is pure (no globals, no time reads) — unit-test it off-target.

**Calibration is executable.** `tools/appraisal_calibrate.py` mirrors the
integer scoring exactly (C truncation included) and asserts 20 canonical
scenarios (winner + margin) plus 7 stability checks. The table in
`appraisal.c` is baked from it; edit the script first, make it pass, then
copy the numbers over.

**Why moods don't flap.** Stability is layered, and measured:
- *Mood* is rate-limited by construction (±2/min through the accumulator,
  hours-scale baseline pull) — no event can swing it.
- *Drives* move by small event deltas, and the weights map them at ≤0.85:1
  (the legacy formulas used 1.0:1 for their dominant terms), so per-event
  score movement shrank.
- *Expressions* are guarded by hold windows (4–12 s), switch margins
  (4–10), and an **incumbency bonus** (+3 to the current expression) that
  offsets the transition-affinity pull toward neighbors (up to +5), which
  previously made adjacent flips cheaper than no hysteresis at all.
- Measured headroom (stability suite): one tap is 16 points short of
  flipping CALM, one notification 8 short, a mid-play arousal wiggle 19
  short — while a burst storm (+54), genuine drowsiness at sleepiness ≈75
  (+7), and post-stimulus recovery to CALM (+15) all still shift cleanly.

## 1b. Carry context: ON_SURFACE / IN_HAND / WORN (keychain reality)

The motion stack resolves `pet_state.carry_context` — see §11 of
`02_SOFTWARE_REQUIREMENTS…` ("pocket_motion") and the keychain product
reality: Kerfus hangs from jeans or a backpack most of the day.

- **WORN detection** (`in_hand_detector.c`): a worn evidence accumulator
  charged by gait cadence and **swing periodicity** (`compute_swing_score`
  in the classifier: zero-crossing regularity of the gravity-projected
  bounce in the 0.7–3 Hz band). Worn is sticky — a keychain on a sitting
  owner is still worn — and exits only to a confirmed surface, an in-hand
  capture, or after a long ambiguous quiet stretch.
- **Grab capture**: while WORN, pickup/in-hand evidence is *gated*. It can
  only accumulate inside a capture window opened by the dangle oscillation
  collapsing below its rolling baseline while orientation stabilizes (a
  hand damps the swing), or by sustained calm hand-like motion. This is
  what makes "grabbed off the bag" fast and reliable while body bounce can
  never become a phantom pickup.
- **Shake sanity while worn**: SHAKE_LIGHT/SHAKE_PLAY are never emitted
  while worn; SHAKE_ROUGH/IMPACT need ~1.4× the energy (real drops still
  register). The wake interrupt stops spamning MOTION_WAKE while worn.
- **Power**: a worn-but-quiet device sits in `MODE_WORN_WATCH` (slow
  classification heartbeat, hw step counter alive) instead of burning
  full-rate active windows on every step.

Worn style is configurable (`CONFIG_KERFUR_WORN_EXPRESSIVE` default +
`kerfur emotion worn <quiet|expressive>`, persisted in emotional memory):

- **quiet** (default): screen rests in the pocket; motion/notification
  display wake-ups and idle quirks are suppressed; steps keep feeding
  mood/boredom; peers, grabs, deliberate touch and the charger still wake
  the face ("First Kerfus meet each other" works from a backpack).
- **expressive**: ambient companionship while dangling; everything wakes
  the face as usual.

Context edges are events (`APP_EVENT_CARRY_CONTEXT_CHANGED`) with small
emotional meaning: clipping on = a bit of adventure; being grabbed off the
bag = warmth + wake blink; set down = settle.

## 1c. Expression transition routing (easing between feelings)

`update_expression` (`behavior_engine.c`) is two layers:

1. **Target selection** — appraisal + hysteresis (§1a, the hold/margin table)
   pick a *stable* target expression. Hysteresis governs only how readily the
   target is allowed to change (anti-flap).
2. **Routing** — the displayed face walks toward the target **one adjacency
   hop at a time** (`expr_first_hop`, BFS over the `transition_affinity`
   graph; CALM is the hub), one hop per `EXPR_ROUTE_HOP_MS` (600 ms, paced by
   the 100 ms tick). Emotionally-distant changes ease through intermediates
   instead of snapping.

**Designed companion arcs** (each a real context; locked as named eases in
`tools/appraisal_calibrate.py` so they can't silently regress):

| Context | Gradation |
| --- | --- |
| Wake from sleep | `ASLEEP → SLEEPY → CALM` (ASLEEP is graph-adjacent only to SLEEPY) |
| Soothe an irritated pet | `ANNOYED → CALM → CURIOUS → HAPPY` (petting sets the target and the face *eases* there, replacing the old instant flip that snapped back when the pet window closed) |
| Overstimulation runs out of energy | `OVERSTIMULATED → DRAINED` — the **burnout crash**; the `OVERSTIMULATED↔DRAINED` edge keeps it out of serene CALM mid-meltdown |
| Exhausted pet drifts off | `DRAINED → SLEEPY → ASLEEP` |
| Overstimulation soothed *while energetic* | `OVERSTIMULATED → PLAYFUL → HAPPY` (same origin as the crash, opposite outcome by context) |
| Loneliness lifts when company arrives | `LONELY → CALM → CURIOUS → HAPPY` |
| Left alone after contentment | `CONTENT → CALM → NEEDY` |
| Winds down into a cozy nap | `CURIOUS → SLEEPY → COZY` |
| Drowses off at night | `CONTENT → … → SLEEPY → ASLEEP` |

Overrides: forced/debug and asleep **snap**; petting and the normal election
**ease**. The micro-reaction layer still overlays instantly (a startle is
immediate; only the underlying expression eases). Validated by
`tools/appraisal_calibrate.py` routing checks (all 169 routes converge in ≤4
hops + named eases). Tuning knobs: `EXPR_ROUTE_HOP_MS`, and the
`transition_affinity` graph (the adjacency edges, incl. `ASLEEP↔SLEEPY` and
the `OVERSTIMULATED↔DRAINED` burnout edge).

Graph-shape note: adjacency is **symmetric** (an edge in either direction makes
two feelings neighbours), and CALM is the hub, so any distant `→ X` route tends
to pass through CALM unless a more specific edge exists. Add edges sparingly —
each one shortens paths and *reduces* easing. The burnout edge is one direction
in scoring (`OVERSTIMULATED → DRAINED` only) so a drained pet is never pulled
back up toward overstimulation; symmetric adjacency is enough for routing.

## 2. Mood (slow valence)

- Events do **not** change mood directly. They call `nudge_mood(delta)`, which
  fills a bounded accumulator (±60). The 60 s tick converts at most **±2 mood
  per minute** out of it; leftover residue evaporates. This rate limit keeps
  mood believable no matter how spammy the inputs are.
- Positive: petting (+4/+5), holds (+2), taps (+1), friend encounters (+8,
  +4 more if the friend is visibly happy), unknown peers (+3), play invites
  (+3), real walks (+2), shared walks with a peer (+1 per glance), charging
  (+2), long encounters ending (+2).
- Negative: rough shaking (−8/−10), impacts (−6), notification overload (−4),
  2 h+ of loneliness (−2 per 30 min).
- Baseline pull: when the accumulator is empty, mood drifts 1 point toward 50
  every 5 minutes (≈ half-day temperament horizon).
- Effects: `mood_bias = (mood−50)/6` (±8) tilts expression scoring
  (HAPPY/CONTENT/PLAYFUL/CALM up, LONELY/NEEDY/ANNOYED down on good days, the
  reverse on bad days), feeds PLAY intent, buffers WITHDRAW intent, and gives
  the face a ±1 px mouth/brow set (see §7).

## 3. Personality

`enum pet_personality` (persisted; append-only): **balanced, curious, shy,
playful, calm**. Each profile is a set of percentage multipliers applied at
specific points (`scale_pct`):

| Profile | warmth | stress | social | play | rest |
|---------|--------|--------|--------|------|------|
| balanced | 100 | 100 | 100 | 100 | 100 |
| curious  | 100 |  95 | 120 | 105 |  90 |
| shy      | 115 | 125 |  70 |  85 | 105 |
| playful  | 100 |  90 | 110 | 130 |  85 |
| calm     | 105 |  70 |  95 |  85 | 115 |

- `warmth` — attachment/trust/comfort gains from touch
- `stress` — stress increments from rough handling / overload
- `social` — social-warmth/curiosity gains from peers and notifications
- `play` — arousal gains from play events and the PLAY intent score
- `rest` — passive sleepiness accumulation per minute

A shy Kerfus startles harder but bonds deeper once petted; a playful one gets
more out of every shake and stays awake longer. Switch at runtime with
`kerfur emotion personality <id>` (persisted immediately).

## 4. Emotional memory (persistence)

`src/behavior/emotion_memory.c`, `CONFIG_KERFUR_EMOTION_MEMORY` (default y,
needs `CONFIG_SETTINGS`). One packed, versioned record at settings key
`kerfur/emo/traits`: personality, attachment, trust, mood, lifetime_pets.

- **Load**: `behavior_engine_init()` → `emotion_memory_init()` loads the
  subtree synchronously (idempotent; tolerant of the later full
  `settings_load()` from `ble_manager_init()`). Restored values are lightly
  compressed toward the middle (attachment/trust → 25..90, mood → 35..70):
  the pet wakes rested but recognizably itself.
- **Save**: throttled in `try_save_emotion_memory()` — at most one write per
  ~25 min, only when traits drifted meaningfully (Δattachment/trust ≥ 3,
  Δmood ≥ 4, Δpets ≥ 10, or personality changed). Extra trigger on
  `CHARGER_CONNECTED` (min 10 min apart); personality changes save
  immediately. NVS wear is negligible at this rate.
- Fast drives (arousal, stress, boredom…) intentionally boot fresh.

## 5. Peer emotional contagion ("First Kerfus meet each other")

The nearby beacon already carries the sender's mode/expression summary
(packed 4+4 bits). The engine now reads it on `PEER_NEAR` and
`ENCOUNTER_START` and classifies the peer's **vibe**:

- **RESTING** (asleep/drowsy/low-power/charging mode, or asleep/sleepy/
  drained/cozy expression) → quiet interest: arousal down, less stimulation,
  greeting downgraded to a bow / quiet glance. Kerfus does not bounce at a
  sleeping friend; meeting a resting friend even adds a little sleepiness
  ("settles in next to it").
- **BRIGHT** (happy/playful/content) → joy is contagious: extra arousal
  (personality-scaled), extra mood; an unknown-but-bright peer can earn a
  happy bounce if Kerfus is in PLAY intent and trusting.
- **STRAINED** (overloaded mode, or annoyed/overstimulated/needy/lonely
  expression) → concern: a little stress, extra curiosity, gentle glance
  instead of celebration.
- **NEUTRAL** (calm/curious) → previous behavior.

Friend hearts still show in all cases (a strained friend is still a friend).
Test via `kerfur nearby inject friend <id> [expr]` etc. — injects default to
an IDLE/CALM peer so existing flows are unchanged.

Privacy note: contagion uses only the already-broadcast anonymous summary;
nothing new is transmitted or stored per peer.

## 6. Sleep inertia and night awareness

- Transitioning out of `PET_MODE_ASLEEP` sets a grogginess window: **45 s**
  (day) / **90 s** (night, from the synced clock). While groggy: SLEEPY
  expression gets +14, REST intent +12, and any HAPPY_BOUNCE is downgraded to
  a small blink. The pet visibly *wakes up*, it doesn't switch on.
- `APP_EVENT_WAKE` at night clears less sleepiness and adds less arousal.
- `IDLE_TIMEOUT` / `DISPLAY_AMBIENT_TIMEOUT` now settle the pet (small
  arousal/stress release) instead of being ignored.

## 7. Idle micro-life (behavior + face)

Two cooperating layers keep Kerfus alive between events without being noisy:

**Behavior level** (`maybe_idle_quirk`, 1 s tick): when the screen is on,
mode is IDLE/DROWSY, no reaction is active, stimulation is low and battery is
fine, the pet performs one small intent-driven gesture every **25–70 s**
(doubled in ambient, +50% when drowsy): LOOK_UP when it wants attention,
glance left/right when curious/observing, slow blink when resting.

**Face level** (`face_runtime.c`, "Stage 5"):
- **Organic blinking** — blink intervals are jittered ±40% and emotion-aware
  (alert/stressed ≈ ×0.6 period, sleepy ≈ ×1.5); ~10% of blinks (22% when
  alert) are **double blinks**; big reactions (startle, bounce, burst) are
  often followed by a quick settle blink.
- **Wandering idle gaze** — when there is no real gaze input (not in hand,
  look confidence ≈ 0) and no reaction, the pupils occasionally ease to a
  random nearby point, hold ~1.2–2.8 s, and return; every 6–16 s in
  foreground, 12–26 s in ambient; sleepy faces usually skip their turn.
  This is now the **only** idle pupil motion: the old constant cyclic
  ambient drift (the up-right-down-left wobble that ran on CALM/CURIOUS/
  CONTENT) was removed (2026-06-15) because it read as fidgety — a resting
  face now holds still between these rare, deliberate glances.
  (`resolve_ambient_pupil_drift` is gone; `update_wander` remains.)
- **Breathing** — a faint, slow 1 px mouth/whisker lift that appears only
  briefly (~18 % of a long 5.5–9 s cycle, slower when sleepy) and continues
  while asleep; suppressed during reactions, transitions, and critical
  battery. Deliberately subtle (reworked 2026-06-15 from the earlier, much
  more active 2.8–5.2 s half-cycle bob). Reaction whisker-wiggles are
  unaffected.
- **Continuous context on the face** — mood sets the resting mouth/brow
  (droop when worn down, slight lift when content); arousal opens the eyes a
  touch, heavy sleepiness drops the lids; charging/low-battery/walking/
  in-hand biases were already present (Stage 4).

All face micro-life is suppressed exactly where it must be: during
reactions, expression transitions, blinking (for gaze), battery-low/critical
(wander, breathing on critical), and sleepy recipes wander only rarely.

## 7a. Render-path robustness + host test

- **A pupil swap must always complete.** `resolve_pupil_swap()` can defer an
  eyeball change behind a blink (`ON_BLINK`) or a short settle (`SETTLE`) so
  the change isn't jarring — but a recipe whose blink profile is disabled
  never closes its eyes. OVERSTIMULATED (spiral pupils, blink disabled,
  swap = `ON_BLINK`) therefore waited forever and never showed its spiral;
  you saw the previous expression's pupils behind overstimulated eye-whites.
  Fixed (2026-06-15): `ON_BLINK` falls back to `SETTLE` when the effective
  blink profile is disabled, so the swap always resolves (~400 ms).
- **Eye-white position glide (2026-06-15):** on an expression change the eye
  whites (and the pupils riding on them) used to *teleport* to the new layout
  — only brows/mouth/openness eased — which looked rough and produced odd
  frames when a blink landed on the jump. They now ease via four transition
  channels (`ch_*_eye_d{x,y}` in `face_runtime.c`, applied in
  `ui_renderer.c`'s `eye_white_draw_position`), like the other features.
- **Host test** (`tests/face_host/`, `bash tests/face_host/run.sh`): compiles
  the real `face_runtime.c` + generated tables against small Zephyr shims and
  drives every expression, transition and reaction (incl. a blink landing
  mid-transition), asserting plan validity, that pupil swaps always complete,
  that pupils never snap/asymmetry across a blink boundary, and that the eye
  glide settles. Engine *selection/routing* is covered separately by
  `tools/appraisal_calibrate.py`; this covers face *rendering*.

## 8. Diagnostics

- `kerfur emotion dump` → logs `Face dump …` + `Emotion dump E=.. Sl=.. At=..
  Bo=.. St=.. Ar=.. So=.. Tr=.. Cu=.. Mo=..(acc=..) ctx=s/c/w intent=..
  groggy=.. pers=.. pets=.. carry=..(..) sit=.. worn_style=..` (the
  CLAUDE.md §15 "emotion print").
- `kerfur emotion personality <0..4>` → switch + persist personality.
- `kerfur emotion worn <quiet|expressive>` → switch + persist worn style.
- `kerfur nearby inject near|friend|unknown … [expr]` → contagion testing.
- `kerfur face carry <in_hand> <pickup> <inhand> <walk> [ctx 0-4]` →
  carry-context injection (3 = WORN) for situation testing without an IMU.
- Motion debug logging (`kerfur face motion conf_log on`) now includes
  `swing= worn=` columns.
- Heartbeat status line (`CONFIG_KERFUR_TRACE_EVENTS`) now includes
  `Ar= So= Mo= pers=`.

## 9. Tuning knobs worth knowing

| What | Where |
|------|-------|
| **Expression weights / situation biases / masks** | `g_appraisal[]`, `g_situations[]` in `src/behavior/appraisal.c` — tune via `tools/appraisal_calibrate.py` first, then bake |
| Expression stickiness | incumbency bonus (+3) in `update_expression`, hold/margin table above it, `transition_affinity` |
| Mood rate limit / accumulator bound | `MOOD_ACCUM_LIMIT`, transfer code in `apply_tick_60s` |
| Mood event nudges | `nudge_mood(...)` calls in `apply_event` |
| Personality table | `g_personalities[]` |
| Grogginess duration | sleep-inertia block in `behavior_engine_handle_event` |
| Idle quirk cadence | `schedule_idle_quirk` (25–70 s base) |
| Save throttle | `EMOTION_SAVE_MIN_INTERVAL_MS`, `memory_changed_enough` |
| Blink cadence/jitter | `next_blink_delay_ms`, double-blink chances in `update_blink_state` |
| Wander range/cadence | `FACE_WANDER_RANGE`, `update_wander` |
| Breathing period | `breathing_offset` |
| Worn enter/exit, grab capture | `WORN_*` / `GRAB_*` in `in_hand_detector.c` |
| Swing periodicity band | `SWING_*` in `motion_classifier.c` |
| Worn heartbeat / burst length | `WORN_POLL_MS`, `WORN_ACTIVE_BURST_MS` |
| Worn shake thresholds | worn-scaled constants in `check_shake` |

## 11. Nearby social architecture: friend recognition + privacy model

(2026-06-12; detailed rationale in `KERFUS_AGENT_HANDOFF.md` §5b.)

### Why the old system broke

The old friend table stored session-only ephemeral IDs.  After any ID rotation
(~15 min) or reboot, friends looked like strangers — `PEER_NEAR` with
`is_friend = false`, no warm greeting, encounter count stuck at zero.  The
emotion engine received incoherent context.

### How the new system works

**Over-the-air ID (unchanged):** `ephemeral_id` = `crc32(device_secret || slot)`,
rotates every `KERFUR_NEARBY_ID_ROTATE_S` seconds.  Nothing persistent is
ever broadcast.

**Friend-layer (new):**

1. Confirmed friends share their `device_secret` via the companion app (stored
   in NVS, never retransmitted).
2. On every incoming beacon, `resolve_friend_id()` checks whether the ephemeral
   ID matches any stored friend key for the current wall-clock slot (±1 for
   boundary tolerance).
3. A match sets `peer->friend_index` (stable slot 0..MAX_FRIENDS-1, -1 = unknown).
4. `friend_index` propagates through: `kerfur_peer` → `app_event_peer` →
   `encounter_record` → `pet_state.current_active_friend_index`.

**Wall-clock dependency:** `resolve_friend_id()` returns -1 if `g_wallclock_valid`
is false.  Clock becomes valid once the companion app sends `APP_EVENT_TIME_SYNC`
(unix timestamp ≥ 946684800); `kerfur_nearby_set_wall_clock()` is called from
`app.c` and immediately rotates the local ID to the wall-clock slot.

**Mid-encounter rotation continuity:**

```
scan sees new ephemeral_id
    ↓
resolve_friend_id() → match → friend_index = F
    ↓
peer_find_friend_locked(F) → existing peer entry found
    ↓
peer->ephemeral_id updated silently (no eviction)
    ↓
emotion engine: no PEER_LOST, no encounter gap, friend_index unchanged
```

**Behavior engine stability:** identity comparisons in `PEER_LOST` and
`ENCOUNTER_END` check `friend_index` first (stable), falling back to
`ephemeral_id` only for non-confirmed peers.

**NVS writes:** encounter-count increments are queued in a bitmask and flushed
by `kerfur_nearby_tick()` *after* `g_peer_mutex` is released — no flash write
while holding the peer lock.

**Lock order:** `g_secret_mutex` → `g_friend_mutex` → `g_peer_mutex`
(`resolve_friend_id` is called *before* peer lock is taken; never reverse this).

### Privacy properties

| Property | Guarantee |
|----------|-----------|
| Over-the-air ID | Ephemeral, rotates every ~15 min; never stable across rotations |
| `device_secret` / `peer_key` | NVS only; never in any beacon or advertisement |
| Recognizability without app | Zero — clock not valid → returns -1 → curious stranger |
| Persistent recognition | Requires: (a) explicit companion-app pairing + (b) both devices clock-synced |
| Retroactive tracking | Impossible — each slot's ID is independently random to any device without the key |

### Diagnostics

```
kerfur nearby peers           -- live peer table (state, rssi, friend_index)
kerfur nearby friends dump    -- stored friends (slot, nickname, encounters, expected_id)
kerfur nearby friends resolve <id>  -- resolve an observed ID → friend slot
kerfur nearby friends add <64hex> [nick]  -- dev/test: add a friend by key
kerfur nearby friends remove <slot>       -- remove + NVS tombstone
kerfur nearby friends test_rotate [slot]  -- inject expected ID, verify resolve works
```

### Limitations / known gaps

- First-use: until both devices have ever connected the app, wall clock is
  invalid and friends appear as curious strangers (correct privacy default).
- Multiple devices with different boot times need the same wall-clock reference
  to predict each other's IDs — companion app is the synchronization point.
- `MAX_FRIENDS` = 8 (Kconfig default); the NVS record is 52 bytes per slot.
  Increase if needed; each added slot adds one `crc32()` call per scanned beacon.

## 10. Invariants (do not break)

- Mood changes only through the accumulator (rate-limited); never write
  `state->mood` directly from an event handler.
- Personality IDs and the emotion-memory record layout are persisted:
  append, never reorder; bump `EMOTION_MEMORY_VERSION` on layout change
  (the flags byte reuses an old reserved byte — old records read as 0 =
  quiet worn style, by design).
- Emotional logic stays out of drivers; contagion reads only the anonymous
  beacon summary already broadcast (privacy rule, CLAUDE.md §12).
- Idle micro-life must stay subtle: long randomized intervals, suppressed on
  low battery and while anything else is happening.
- Appraisal calibration: every weight row stays on the shared envelope
  (bias + positive weights ≤ ~135); `appraisal.c` stays pure (no globals,
  no clock) so it remains unit-testable.
- While WORN, pickup/in-hand evidence may only accumulate inside a grab
  window; SHAKE_LIGHT/PLAY are never emitted worn. Body bounce must never
  become a pickup, a play shake, or a full-rate sampling loop.
- `enum pet_mode` values ride in the nearby beacon (4 bits) — append only
  (PET_MODE_WORN = 9).
