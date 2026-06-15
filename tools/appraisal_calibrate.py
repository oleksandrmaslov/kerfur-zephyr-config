#!/usr/bin/env python3
"""Appraisal weight calibration / regression harness for Kerfus.

Mirrors the integer scoring math of src/behavior/appraisal.c exactly
(C division truncates toward zero) and runs a suite of canonical
emotional scenarios. Each scenario states which expression must win the
appraisal and by what minimum margin over the runner-up.

Usage:
    python tools/appraisal_calibrate.py            # run the suite
    python tools/appraisal_calibrate.py --table    # dump score tables

When tuning: edit WEIGHTS / BIAS / SITUATION_BIAS here first, make the
suite pass, then copy the numbers into src/behavior/appraisal.c (and
keep both in sync — this file is the executable spec for that table).
"""

import argparse
import sys

# ── Expression / feature / situation vocabulary (mirror pet_state.h) ──

EXPRS = [
    "CALM", "CURIOUS", "CONTENT", "HAPPY", "PLAYFUL", "SLEEPY",
    "NEEDY", "LONELY", "ANNOYED", "OVERSTIMULATED", "COZY",
    "DRAINED", "ASLEEP",
]

FEATURES = [
    "ENERGY", "SLEEPINESS", "ATTACHMENT", "BOREDOM", "STRESS", "AROUSAL",
    "SOCIAL_LOAD", "TRUST", "CURIOSITY", "MOOD_HIGH", "MOOD_LOW",
    "COMFORT", "STIMULATION", "SOCIAL_WARMTH", "FRESH_PET", "RECENT_PET",
    "RECENT_MOTION", "RECENT_NOTIF", "ROUGH_RECENT", "NEGLECT",
    "LONG_DISCONNECT", "NIGHT", "GROGGY", "BATT_LOW", "BATT_CRITICAL",
    "CHARGING", "WALK_NOVELTY", "BURST",
]

SITUATIONS = ["RESTING", "ENGAGED", "WORN_QUIET", "WORN_LIVELY",
              "CHARGING", "SOCIAL"]

# ── Weight table (the numbers under calibration) ─────────────────────

BIAS = {
    "CALM": 52, "CURIOUS": 6, "CONTENT": 6, "HAPPY": 0, "PLAYFUL": 0,
    "SLEEPY": 0, "NEEDY": 6, "LONELY": 0, "ANNOYED": 8,
    "OVERSTIMULATED": 0, "COZY": 8, "DRAINED": 24, "ASLEEP": -100,
}

WEIGHTS = {
    "CALM": {"STRESS": -85, "SOCIAL_LOAD": -40, "BOREDOM": -22,
             "ENERGY": 22, "MOOD_HIGH": 10, "MOOD_LOW": -8,
             "STIMULATION": -12, "NIGHT": 4},
    "CURIOUS": {"CURIOSITY": 62, "RECENT_NOTIF": 14, "RECENT_MOTION": 11,
                "WALK_NOVELTY": 16, "STIMULATION": 10, "SLEEPINESS": -30},
    "CONTENT": {"ATTACHMENT": 40, "TRUST": 38, "RECENT_PET": 16,
                "STRESS": -45, "MOOD_HIGH": 12, "MOOD_LOW": -8,
                "COMFORT": 18},
    "HAPPY": {"ATTACHMENT": 42, "ENERGY": 24, "RECENT_PET": 22,
              "FRESH_PET": 26, "STRESS": -45, "MOOD_HIGH": 16,
              "MOOD_LOW": -12, "COMFORT": 18, "SOCIAL_WARMTH": 14,
              "AROUSAL": 10},
    "PLAYFUL": {"AROUSAL": 75, "TRUST": 28, "WALK_NOVELTY": 12,
                "MOOD_HIGH": 10, "MOOD_LOW": -8, "BATT_LOW": -20,
                "STIMULATION": 10, "SLEEPINESS": -15},
    "SLEEPY": {"SLEEPINESS": 78, "AROUSAL": -18, "NIGHT": 8,
               "NEGLECT": 10, "GROGGY": 14, "CHARGING": 6},
    "NEEDY": {"BOREDOM": 55, "ATTACHMENT": 18, "RECENT_PET": -20,
              "NEGLECT": 18, "MOOD_LOW": 10, "MOOD_HIGH": -6,
              "SOCIAL_WARMTH": -22, "COMFORT": -16},
    "LONELY": {"BOREDOM": 48, "LONG_DISCONNECT": 20, "RECENT_MOTION": -10,
               "NEGLECT": 26, "MOOD_LOW": 14, "MOOD_HIGH": -10,
               "SOCIAL_WARMTH": -22, "COMFORT": -14},
    "ANNOYED": {"STRESS": 70, "ROUGH_RECENT": 24, "TRUST": -16,
                "MOOD_LOW": 8, "MOOD_HIGH": -6, "STIMULATION": 14},
    "OVERSTIMULATED": {"SOCIAL_LOAD": 60, "BURST": 35, "AROUSAL": 25,
                       "STIMULATION": 18},
    "COZY": {"CHARGING": 52, "STRESS": -30, "RECENT_PET": 10,
             "COMFORT": 24, "SLEEPINESS": 8, "NIGHT": 4},
    "DRAINED": {"BATT_CRITICAL": 62, "BATT_LOW": 24, "ENERGY": -24,
                "SLEEPINESS": 28},
    "ASLEEP": {},
}

SITUATION_BIAS = {
    "RESTING": {"CALM": 6, "SLEEPY": 4},
    "ENGAGED": {"CURIOUS": 8, "HAPPY": 6, "PLAYFUL": 6, "SLEEPY": -10,
                "LONELY": -20, "NEEDY": -10},
    "WORN_QUIET": {"CALM": 8, "CONTENT": 4},
    "WORN_LIVELY": {"CURIOUS": 6, "PLAYFUL": 4, "LONELY": -15,
                    "NEEDY": -10},
    "CHARGING": {"COZY": 10, "CALM": 6, "SLEEPY": 4, "PLAYFUL": -10},
    "SOCIAL": {"CURIOUS": 10, "HAPPY": 8, "PLAYFUL": 3, "SLEEPY": -10,
               "LONELY": -25, "NEEDY": -15},
}

ALLOWED = {
    "WORN_QUIET": {"CALM", "CONTENT", "CURIOUS", "SLEEPY", "DRAINED",
                   "ASLEEP"},
}

# Intent alignment bonuses (mirror intent_alignment_bonus in the engine),
# expressed as (expr -> divisor of intent strength).
INTENT_BONUS = {
    "REST": {"SLEEPY": 6, "COZY": 7, "CALM": 9},
    "PLAY": {"PLAYFUL": 5, "HAPPY": 7},
    "OBSERVE": {"CURIOUS": 5, "CALM": 9},
    "SEEK_ATTENTION": {"NEEDY": 6, "LONELY": 8},
    "WITHDRAW": {"ANNOYED": 6, "OVERSTIMULATED": 7},
    "SELF_SOOTHE": {"CONTENT": 6, "CALM": 7},
}


def trunc_div(a, b):
    """C-style integer division (truncate toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


# Transition affinity (mirror transition_affinity in behavior_engine.c):
# small bonus for expressions adjacent to the current one.
AFFINITY = {
    "SLEEPY": {"CALM": 5, "CURIOUS": 3},
    "CALM": {"CURIOUS": 4, "CONTENT": 4, "SLEEPY": 3},
    "CURIOUS": {"PLAYFUL": 4, "CALM": 3, "HAPPY": 3},
    "PLAYFUL": {"HAPPY": 5, "CURIOUS": 3, "OVERSTIMULATED": 3},
    "HAPPY": {"CONTENT": 5, "PLAYFUL": 3},
    "CONTENT": {"CALM": 4, "HAPPY": 3},
    "ANNOYED": {"CALM": 4, "OVERSTIMULATED": 3},
    "OVERSTIMULATED": {"ANNOYED": 4, "CALM": 3},
    "NEEDY": {"LONELY": 4, "CALM": 3},
    "LONELY": {"NEEDY": 3, "CALM": 4},
    "COZY": {"SLEEPY": 4, "CONTENT": 3},
    "DRAINED": {"SLEEPY": 5, "COZY": 3},
    # Only used for transition routing (waking eases ASLEEP -> SLEEPY -> …);
    # while truly asleep the mode override owns the expression.
    "ASLEEP": {"SLEEPY": 5},
}


# ── Expression transition routing (mirror behavior_engine.c) ─────────
#
# The appraisal + hysteresis layer picks a stable *target* feeling; the
# displayed expression then walks toward it one adjacency hop at a time, so
# emotionally-distant changes ease through intermediates (CALM is the hub)
# instead of snapping. This mirrors expr_adjacent()/expr_first_hop().

def expr_adjacent(a, b):
    return (a == b) or (AFFINITY.get(a, {}).get(b, 0) > 0) or \
        (AFFINITY.get(b, {}).get(a, 0) > 0)


def expr_first_hop(frm, to):
    """BFS one hop from `frm` toward `to` (== `to` when adjacent)."""
    if frm == to or expr_adjacent(frm, to):
        return to
    seen = {frm}
    queue = []  # (node, first_hop_that_reached_it)
    for e in EXPRS:
        if e != frm and expr_adjacent(frm, e):
            seen.add(e)
            queue.append((e, e))
    head = 0
    while head < len(queue):
        cur, fh = queue[head]
        head += 1
        if cur == to:
            return fh
        for e in EXPRS:
            if e not in seen and expr_adjacent(cur, e):
                seen.add(e)
                queue.append((e, fh))
    return "CALM"  # guaranteed fallback (graph is connected)


def route_path(frm, to, limit=20):
    """The full sequence of displayed expressions from `frm` to `to`."""
    path = [frm]
    while path[-1] != to and len(path) < limit:
        nxt = expr_first_hop(path[-1], to)
        if nxt == path[-1]:
            break
        path.append(nxt)
    return path

# Incumbency: the current expression's stickiness against affinity pull
# (mirror EXPR_INCUMBENCY_BONUS in behavior_engine.c).
INCUMBENCY = 3


def score(expr, situation, feats, intent=None, intent_strength=0):
    acc = 0
    for f, w in WEIGHTS[expr].items():
        acc += w * feats.get(f, 0)
    s = BIAS[expr] + SITUATION_BIAS[situation].get(expr, 0) + trunc_div(acc, 100)
    if intent and expr in INTENT_BONUS.get(intent, {}):
        s += trunc_div(intent_strength, INTENT_BONUS[intent][expr])
    return s


def allowed(expr, situation):
    if expr == "CALM":
        return True
    return expr in ALLOWED.get(situation, set(EXPRS))


def rank(situation, feats, intent=None, intent_strength=0, current=None):
    """Full engine model. With `current`, adds transition affinity and
    the incumbency bonus exactly like update_expression()."""
    rows = []
    for e in EXPRS:
        if not allowed(e, situation):
            rows.append((-1000, e))
            continue
        s = score(e, situation, feats, intent, intent_strength)
        if current is not None:
            s += AFFINITY.get(current, {}).get(e, 0)
            if e == current:
                s += INCUMBENCY
        rows.append((s, e))
    rows.sort(reverse=True)
    return rows


def margin_to_switch(situation, feats, current, intent=None, strength=0,
                     hysteresis_margin=4):
    """How far the best challenger is from actually displacing `current`,
    including the engine's post-hold margin (margin - 2 floor check)."""
    rows = rank(situation, feats, intent, strength, current=current)
    cur = next(s for s, e in rows if e == current)
    challenger_score, challenger = next(
        (s, e) for s, e in rows if e != current)
    # The engine switches only if best >= curr + (margin - 2) after the
    # hold window (and >= curr + margin inside it).
    needed = cur + (hysteresis_margin - 2)
    return challenger, challenger_score - needed


# ── Scenario suite ────────────────────────────────────────────────────

BOOT = {  # behavior_engine_init seed, afterglow empty, nothing recent
    "ENERGY": 72, "SLEEPINESS": 22, "ATTACHMENT": 50, "BOREDOM": 20,
    "STRESS": 18, "AROUSAL": 30, "SOCIAL_LOAD": 10, "TRUST": 55,
    "CURIOSITY": 45, "MOOD_HIGH": 4, "MOOD_LOW": 0,
}


def F(**over):
    feats = dict.fromkeys(FEATURES, 0)
    feats.update(BOOT)
    feats.update(over)
    return feats


# (name, situation, features, intent, strength, expected winner,
#  min margin over runner-up, allowed runners-up that may sit close)
SCENARIOS = [
    ("boot idle desk", "RESTING", F(), None, 0,
     "CALM", 8, ()),

    ("deep sleepy night", "RESTING",
     F(SLEEPINESS=85, AROUSAL=12, NEGLECT=100, NIGHT=100, ENERGY=40),
     "REST", 60, "SLEEPY", 10, ()),

    ("drowsy evening", "RESTING",
     F(SLEEPINESS=70, AROUSAL=22, NEGLECT=40, NIGHT=100),
     None, 0, "SLEEPY", 3, ("CALM",)),

    ("fresh petting (30 s after, warm)", "ENGAGED",
     F(ATTACHMENT=72, TRUST=68, STRESS=8, RECENT_PET=95, FRESH_PET=0,
       COMFORT=55, STIMULATION=25, AROUSAL=45, MOOD_HIGH=24),
     None, 0, "HAPPY", 4, ("CONTENT",)),

    ("after petting (8 min, settled)", "RESTING",
     F(ATTACHMENT=70, TRUST=68, STRESS=8, RECENT_PET=20, COMFORT=10,
       AROUSAL=25, MOOD_HIGH=20),
     None, 0, "CONTENT", 3, ("CALM", "HAPPY")),

    ("playful in hand", "ENGAGED",
     F(AROUSAL=70, TRUST=62, STIMULATION=42, ENERGY=70, CURIOSITY=55,
       RECENT_MOTION=80, MOOD_HIGH=16),
     "PLAY", 60, "PLAYFUL", 5, ()),

    ("rough handled", "RESTING",
     F(STRESS=62, ROUGH_RECENT=90, TRUST=32, MOOD_LOW=28, AROUSAL=55,
       STIMULATION=60, RECENT_MOTION=100),
     "WITHDRAW", 50, "ANNOYED", 8, ()),

    ("notification overload", "RESTING",
     F(SOCIAL_LOAD=72, BURST=60, AROUSAL=62, STIMULATION=75,
       RECENT_NOTIF=100, STRESS=42, CURIOSITY=50),
     "WITHDRAW", 40, "OVERSTIMULATED", 6, ()),

    ("lonely afternoon", "RESTING",
     F(BOREDOM=70, NEGLECT=100, LONG_DISCONNECT=100, MOOD_LOW=24,
       SLEEPINESS=35, ENERGY=55),
     "SEEK_ATTENTION", 50, "LONELY", 4, ("NEEDY",)),

    ("needy but connected", "RESTING",
     F(BOREDOM=62, NEGLECT=65, ATTACHMENT=72, MOOD_LOW=12,
       SLEEPINESS=30, ENERGY=60),
     "SEEK_ATTENTION", 45, "NEEDY", 3, ("LONELY",)),

    ("charging cozy", "CHARGING",
     F(CHARGING=100, COMFORT=40, STRESS=10, SLEEPINESS=48, AROUSAL=18),
     "REST", 40, "COZY", 6, ()),

    ("battery low and tired", "RESTING",
     F(BATT_LOW=100, ENERGY=28, SLEEPINESS=60, AROUSAL=18, NEGLECT=60),
     None, 0, "DRAINED", 3, ("SLEEPY",)),

    ("battery critical", "RESTING",
     F(BATT_CRITICAL=100, BATT_LOW=100, ENERGY=8, SLEEPINESS=70,
       AROUSAL=10),
     None, 0, "DRAINED", 15, ()),

    ("fresh walk, curious", "WORN_LIVELY",
     F(WALK_NOVELTY=100, CURIOSITY=62, STIMULATION=30, RECENT_MOTION=100,
       AROUSAL=45, ENERGY=68),
     "OBSERVE", 50, "CURIOUS", 5, ()),

    ("unknown kerfur nearby", "SOCIAL",
     F(CURIOSITY=68, SOCIAL_WARMTH=35, STIMULATION=38, AROUSAL=48,
       RECENT_MOTION=50, SOCIAL_LOAD=20),
     "OBSERVE", 55, "CURIOUS", 5, ()),

    ("friend kerfur encounter", "SOCIAL",
     F(CURIOSITY=55, SOCIAL_WARMTH=70, COMFORT=35, STIMULATION=40,
       AROUSAL=52, ATTACHMENT=68, TRUST=62, MOOD_HIGH=30, ENERGY=65),
     None, 0, "HAPPY", 3, ("CURIOUS", "CONTENT", "PLAYFUL")),

    ("worn quiet, content day", "WORN_QUIET",
     F(ATTACHMENT=60, TRUST=60, MOOD_HIGH=14, SLEEPINESS=35,
       RECENT_MOTION=100, STIMULATION=10),
     None, 0, "CALM", 3, ("CONTENT",)),

    ("groggy wake-up", "RESTING",
     F(SLEEPINESS=74, AROUSAL=34, GROGGY=100, STIMULATION=15,
       RECENT_MOTION=100),
     "REST", 40, "SLEEPY", 6, ()),

    ("stressed then comforted", "ENGAGED",
     F(STRESS=42, COMFORT=60, ATTACHMENT=62, TRUST=60, RECENT_PET=75,
       FRESH_PET=0, AROUSAL=35, MOOD_HIGH=8),
     "SELF_SOOTHE", 45, "CONTENT", 3, ("HAPPY",)),

    ("bored midday poke-me", "RESTING",
     F(BOREDOM=48, NEGLECT=45, CURIOSITY=30, SLEEPINESS=30, ENERGY=60),
     None, 0, "NEEDY", -2, ("CALM",)),  # near-tie with CALM is fine
]


# ── Stability suite: "is Kerfus too easy to shift?" ─────────────────
#
# Each entry: (name, situation, base feats, current expr, perturbation
# dict, hysteresis margin for that arousal band, expect_flip).
# A perturbation models one typical event's drive/afterglow deltas.

STABILITY = [
    # A single tap on a calm pet must not flip the face (the BLINK
    # micro-reaction is the acknowledgement).
    ("calm + one tap", "RESTING", F(), "CALM",
     dict(AROUSAL=32, BOREDOM=19, CURIOSITY=47, STIMULATION=5,
          COMFORT=3, RECENT_MOTION=100), 8, False),

    # One quiet notification: interest may show, but it must not flip
    # CALM instantly (NOTIF_PING covers the moment).
    ("calm + one notification", "RESTING", F(), "CALM",
     dict(CURIOSITY=51, AROUSAL=34, SOCIAL_LOAD=15, STIMULATION=8,
          RECENT_NOTIF=100), 8, False),

    # ...but after a burst-driven stretch the shift is legitimate.
    ("calm + burst storm flips", "RESTING", F(), "CALM",
     dict(SOCIAL_LOAD=55, BURST=48, AROUSAL=55, STIMULATION=60,
          RECENT_NOTIF=100, STRESS=35), 8, True),

    # Mid-play wiggle: arousal dipping 70->60 with fading stimulation
    # must not bounce PLAYFUL away (margin 4 band = high arousal).
    ("playful survives a wiggle", "ENGAGED",
     F(AROUSAL=60, TRUST=62, STIMULATION=22, ENERGY=70, CURIOSITY=55,
       RECENT_MOTION=70, MOOD_HIGH=16), "PLAYFUL",
     dict(), 4, False),

    # Drowsy boundary: at sleepiness 60 a CALM pet stays CALM...
    ("calm holds at sleepiness 60", "RESTING",
     F(SLEEPINESS=60, AROUSAL=22, NEGLECT=30), "CALM",
     dict(), 8, False),

    # ...and by 75 it genuinely gets sleepy (shift must stay possible).
    ("sleepy wins by sleepiness 75", "RESTING",
     F(SLEEPINESS=75, AROUSAL=18, NEGLECT=50), "CALM",
     dict(), 8, True),

    # Recovery: 35 s after the single notification (recency gone,
    # afterglow decayed), CURIOUS must hand the face back to CALM.
    ("curious returns to calm", "RESTING",
     F(CURIOSITY=49, AROUSAL=28, SOCIAL_LOAD=12), "CURIOUS",
     dict(), 8, True),
]


def run_stability(verbose=False):
    failures = 0
    print("--- stability (single events must not flip the face) ---")
    for (name, sit, base, current, pert, hyst, expect_flip) in STABILITY:
        feats = dict(base)
        feats.update(pert)
        challenger, gap = margin_to_switch(sit, feats, current,
                                           hysteresis_margin=hyst)
        flips = gap >= 0
        ok = flips == expect_flip
        status = "ok  " if ok else "FAIL"
        if not ok:
            failures += 1
        verdict = "flips" if flips else "holds"
        print(f"[{status}] {name:34s} {current} {verdict} "
              f"(challenger {challenger}, gap {gap:+d}, "
              f"want {'flip' if expect_flip else 'hold'})")
        if verbose or not ok:
            rows = rank(sit, feats, current=current)
            top = ", ".join(f"{e}={s}" for s, e in rows[:5])
            print(f"        {top}")
    return failures


def run_paths(verbose=False):
    failures = 0
    print("--- transition routing (faces ease between distant feelings) ---")

    # 1. Every from->to pair must converge on `to` within a few hops
    #    (proves the adjacency graph is connected and routes terminate).
    unreachable = 0
    longest = 0
    for frm in EXPRS:
        for to in EXPRS:
            path = route_path(frm, to)
            longest = max(longest, len(path))
            if path[-1] != to or len(path) > 5:
                unreachable += 1
                failures += 1
                print(f"[FAIL] route {frm}->{to}: {path}")
    print(f"[{'ok  ' if unreachable == 0 else 'FAIL'}] all "
          f"{len(EXPRS) * len(EXPRS)} routes converge (longest {longest} nodes)")

    # 2. Named eases — the owner-reported rough cases must now step through
    #    an intermediate instead of snapping.
    named = [
        ("soothe angry -> happy via calm", "ANNOYED", "HAPPY",
         lambda p: p[1] == "CALM" and p[-1] == "HAPPY" and len(p) >= 3),
        ("wake eases out of sleep", "ASLEEP", "CALM",
         lambda p: p == ["ASLEEP", "SLEEPY", "CALM"]),
        ("wake-to-curious still via sleepy", "ASLEEP", "CURIOUS",
         lambda p: p[1] == "SLEEPY" and p[-1] == "CURIOUS"),
        ("drained warms up via sleepy", "DRAINED", "HAPPY",
         lambda p: p[1] == "SLEEPY" and p[-1] == "HAPPY"),
        ("overstimulated -> content via calm", "OVERSTIMULATED", "CONTENT",
         lambda p: p[1] == "CALM" and p[-1] == "CONTENT"),
        ("adjacent stays a single hop", "CALM", "CURIOUS",
         lambda p: p == ["CALM", "CURIOUS"]),
        ("same target is a no-op", "HAPPY", "HAPPY",
         lambda p: p == ["HAPPY"]),
    ]
    for name, frm, to, pred in named:
        path = route_path(frm, to)
        ok = pred(path)
        if not ok:
            failures += 1
        print(f"[{'ok  ' if ok else 'FAIL'}] {name:38s} {' -> '.join(path)}")
    return failures


def run_suite(verbose=False):
    failures = 0
    for (name, sit, feats, intent, strength, want, margin, near_ok) \
            in SCENARIOS:
        rows = rank(sit, feats, intent, strength)
        win_score, winner = rows[0]
        runner_score, runner = rows[1]
        ok = winner == want and (win_score - runner_score) >= margin
        if winner == want and runner in near_ok and \
                (win_score - runner_score) >= 0:
            ok = ok or (win_score - runner_score) >= min(margin, 0)
        status = "ok  " if ok else "FAIL"
        if not ok:
            failures += 1
        print(f"[{status}] {name:34s} ({sit:11s}) -> "
              f"{winner} {win_score} | {runner} {runner_score} "
              f"(want {want} +{margin})")
        if verbose or not ok:
            top = ", ".join(f"{e}={s}" for s, e in rows[:5])
            print(f"        {top}")
    print()
    failures += run_stability(verbose)
    print()
    failures += run_paths(verbose)
    print()
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print(f"all {len(SCENARIOS)} scenarios + {len(STABILITY)} "
          f"stability checks + routing pass")
    return 0


def dump_tables():
    for sit in SITUATIONS:
        print(f"=== {sit} (boot state) ===")
        for s, e in rank(sit, F()):
            print(f"  {e:15s} {s}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.table:
        dump_tables()
        return 0
    return run_suite(args.verbose)


if __name__ == "__main__":
    sys.exit(main())
