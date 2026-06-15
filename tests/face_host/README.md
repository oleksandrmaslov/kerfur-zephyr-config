# Face runtime host test

Off-target render-path test for the Kerfur face pipeline. It compiles the
**real** [`src/ui/face_runtime.c`](../../src/ui/face_runtime.c) and the
generated recipe/asset tables against tiny Zephyr shims (`shim/`) and drives
every expression, transition, and micro-reaction through
`face_runtime_step()`, asserting the produced render plan is coherent.

This is the face-side counterpart to
[`tools/appraisal_calibrate.py`](../../tools/appraisal_calibrate.py) (which
covers engine *selection*): this test covers face *rendering*.

## Run

```bash
bash tests/face_host/run.sh
```

Needs only a host C compiler (`gcc`/`clang`) — no Zephyr toolchain. Override
with `CC=clang bash tests/face_host/run.sh`.

## What it proves

- **Expression identity** — each of the 13 expressions renders its own recipe.
- **Plan validity (every frame)** — all asset/enum ids in range, indicator and
  effect counts bounded, eye openness ≤ 100, pupil offsets stay near the eye.
- **Pupil swaps always complete** — for every `from → to` expression pair, the
  rendered eyeball settles to the destination recipe's eyeball and never gets
  stuck mid-swap. This is the regression guard for the OVERSTIMULATED spiral
  deadlock (a `swap-on-blink` recipe whose blink profile was disabled waited
  forever for a blink that never came). Disable the fix in
  `resolve_pupil_swap()` and this test goes red on every `* → OVERSTIMULATED`.
- **Reactions stay valid** — every `REACTION_*` overlaid on a resting face
  yields a valid plan and clears cleanly.

## Layout

```
tests/face_host/
  test_face_runtime.c   the checks
  run.sh                build + run (gcc, project headers + shims)
  shim/zephyr/...       minimal log / random / sys-util headers for the host
```

The shims are host-only; on device the real Zephyr headers are used. If
`face_runtime.c` grows a new Zephyr dependency, add a matching shim here.
