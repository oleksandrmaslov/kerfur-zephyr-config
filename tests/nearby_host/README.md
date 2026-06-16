# Nearby peer-state host test

Off-target golden-master for the Kerfur **nearby** subsystem — the peer
detection state machine that decides when another Kerfur is `SEEN` / `NEAR` /
`INTERACTING` and when an encounter ends.

It compiles the **real** [`src/nearby/kerfur_nearby.c`](../../src/nearby/kerfur_nearby.c)
and [`src/nearby/encounter_log.c`](../../src/nearby/encounter_log.c) against the
small Zephyr shims in [`shim/`](shim/), replaces the event bus with a capture
sink, and drives synthetic scan candidates + ticks. No Zephyr toolchain or
hardware needed.

```bash
bash tests/nearby_host/run.sh
```

## What it pins

- `SEEN -> NEAR -> INTERACTING` rise, then idle-out to `ENCOUNTER_END` / cooldown
- `SEEN -> CHECKING` (no `NEAR`) when RSSI is below the NEAR threshold
- `NEAR` idle timeout -> `PEER_LOST` with no encounter
- own-beacon echo is ignored
- the `MAX_ACTIVE_ENCOUNTERS` prune (simultaneous NEAR/INTERACTING cap)
- stranger payload shape (not a friend, `friend_index == -1`, `session_encounters`)

It is a **golden master**: it locks today's detection behavior so the upcoming
social-handshake / encounter-typing work can't silently regress it. When that
work lands, extend this file with the new assertions (received `social_event`
-> engine events, encounter type classification) rather than loosening these.

## Counterpart suites

- Engine selection/routing — [`tools/appraisal_calibrate.py`](../../tools/appraisal_calibrate.py)
- Face rendering — [`tests/face_host/`](../face_host/)
