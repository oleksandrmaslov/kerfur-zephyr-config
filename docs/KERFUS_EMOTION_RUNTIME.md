# Kerfus Emotion Runtime

> How the emotional core works after the 2026-06-11 rework.
> Code: `src/behavior/behavior_engine.c`, `src/behavior/emotion_memory.c`,
> `src/behavior/micro_reaction.c`, `src/ui/face_runtime.c`.
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
| Persisted traits | settings/NVS via `emotion_memory.c` | across reboots | Attachment, trust, mood, lifetime petting count, personality — "it is still the same pet" |
| Intent | runtime (`REST/SEEK_ATTENTION/OBSERVE/PLAY/SELF_SOOTHE/WITHDRAW`) | ~5 s resolution, hysteresis | A behavioral "leaning" that biases expression choice and idle behavior |

Doc-vocabulary mapping (CLAUDE.md §7 ↔ code): mood↔mood, affection↔attachment,
social_interest↔social_load/social_warmth, alertness↔arousal,
tiredness/sleepiness↔sleepiness, plus boredom/trust/curiosity as extensions.

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
  foreground, 12–26 s in ambient; sleepy faces usually skip their turn. This
  path also finally feeds the recipes' configured **ambient pupil drift**
  into the rendered pupils (previously computed but never applied).
- **Breathing** — a slow 1 px mouth/whisker bob (period 2.8–5.2 s, slower
  when sleepy) that continues while asleep; suppressed during reactions,
  transitions, and critical battery.
- **Continuous context on the face** — mood sets the resting mouth/brow
  (droop when worn down, slight lift when content); arousal opens the eyes a
  touch, heavy sleepiness drops the lids; charging/low-battery/walking/
  in-hand biases were already present (Stage 4).

All face micro-life is suppressed exactly where it must be: during
reactions, expression transitions, blinking (for gaze), battery-low/critical
(wander, breathing on critical), and sleepy recipes wander only rarely.

## 8. Diagnostics

- `kerfur emotion dump` → logs `Face dump …` + `Emotion dump E=.. Sl=.. At=..
  Bo=.. St=.. Ar=.. So=.. Tr=.. Cu=.. Mo=..(acc=..) ctx=s/c/w intent=..
  groggy=.. pers=.. pets=..` (the CLAUDE.md §15 "emotion print").
- `kerfur emotion personality <0..4>` → switch + persist personality.
- `kerfur nearby inject near|friend|unknown … [expr]` → contagion testing.
- Heartbeat status line (`CONFIG_KERFUR_TRACE_EVENTS`) now includes
  `Ar= So= Mo= pers=`.

## 9. Tuning knobs worth knowing

| What | Where |
|------|-------|
| Mood rate limit / accumulator bound | `MOOD_ACCUM_LIMIT`, transfer code in `apply_tick_60s` |
| Mood event nudges | `nudge_mood(...)` calls in `apply_event` |
| Personality table | `g_personalities[]` |
| Grogginess duration | sleep-inertia block in `behavior_engine_handle_event` |
| Idle quirk cadence | `schedule_idle_quirk` (25–70 s base) |
| Save throttle | `EMOTION_SAVE_MIN_INTERVAL_MS`, `memory_changed_enough` |
| Blink cadence/jitter | `next_blink_delay_ms`, double-blink chances in `update_blink_state` |
| Wander range/cadence | `FACE_WANDER_RANGE`, `update_wander` |
| Breathing period | `breathing_offset` |

## 10. Invariants (do not break)

- Mood changes only through the accumulator (rate-limited); never write
  `state->mood` directly from an event handler.
- Personality IDs and the emotion-memory record layout are persisted:
  append, never reorder; bump `EMOTION_MEMORY_VERSION` on layout change.
- Emotional logic stays out of drivers; contagion reads only the anonymous
  beacon summary already broadcast (privacy rule, CLAUDE.md §12).
- Idle micro-life must stay subtle: long randomized intervals, suppressed on
  low battery and while anything else is happening.
