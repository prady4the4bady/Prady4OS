# DDR-965 — `smoke-cadence`: the test knob shrinks the period but not the animation

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## 1. The capture this is built on

DDR-964's sibling instrument (`PRADYOS_CAD_ADV`, commit `544538b`) was added so
the next `smoke-cadence` red would name its own remedy. It did, on `992b336`,
shard 5:

```text
PRADYOS_CAD_ADV n=1 elapsed_ms=10520 target_ms=2000
PRADYOS_CAD_ADV n=2 elapsed_ms=10880 target_ms=2000
PRADYOS_CAD_ADV n=3 elapsed_ms=17990 target_ms=2000
[cadence] FAIL — no full auto cycle
```

**CI reached n=3 of the 4 required.** It ran out of wall-clock budget one
advance short. This is not a hypothesis: the gate needs four advances, three
happened, and the fourth did not fit in `timeout 120`.

What the same capture rules out:
- **Not a stopped clock** — advances occur, and `PRADYOS_PRETRANSITION` fires.
- **Not guest starvation** — `[hb]` reaches t=11500 with the guest healthy.
- **Not an SMP or console-lock effect** — the log shows `rqcpus=1 rqq=1
  rqpres=1`; this gate is single-CPU, so DDR-963's line lock has no cross-CPU
  contention here and is not implicated. (`992b336` is the commit that added it,
  which is exactly why this had to be checked rather than assumed.)

## 2. Why the period cannot reach its target

`cadence_tick()` runs once per FRAME, and each advance renders an animation
before the next period can be observed:

| cost | frames |
|---|---|
| `set_ambiance((g_cur_amb + 1) & 3, 12)` | 12 render+present |
| pre-transition pulse (`f = 1..3` plus a settle) | ~4 render+present |

So an advance cannot complete in less than ~16 render+present pairs, whatever
the cadence knob says. Measured locally over three passing runs / 18 advances,
that floor is **~11.3 s**, a plateau — against a `target_ms` of 2000.

The `'k'` hotkey's own comment states the intent it has never met:

> 2 s per ambiance => a full 4-ambiance cycle in ~8 s, comfortably inside the
> gate window even on a loaded TCG runner

Four advances actually need ~34–45 s locally, and on the failing runner three
needed ~39 s with the third alone taking 18 s. **The knob shrinks the period but
not the animation, and the animation is the floor.**

## 3. Decision — shrink the animation AND widen the window

### A correction to this DDR's own first draft

This section originally read "shrink the animation under the test knob, **not**
the window", and rejected widening `timeout 120` as merely moving a marginal
boundary. **Measurement refuted that, and the reasoning behind it was wrong.**

Shrinking the transition to 2 frames was implemented and measured over three
runs. The period did **not** settle at one value:

| run | steady period |
|---|---|
| A (idle host) | ~3.08 s, reaching n=28 in the window |
| B (loaded host) | ~9.7 s, n=7 |
| C (loaded host) | ~9.9 s, n=7 |

Against a ~11.3 s pre-fix plateau that is a real improvement, but it is
load-dependent and nowhere near the 2000 ms target. **So the animation was not
the floor.** `cadence_tick()` is called once per main-loop frame, so the period
is quantised to the *main loop's* frame interval; the transition frames are
extra renders inside one advance, not the thing that gates the next one.

### The budget the capture actually shows

Re-reading the failing CI log for the arithmetic rather than the trend:
`[hb] t=6500` is the line immediately before `PRADYOS_CAD_ADV n=1`, and the last
heartbeat is `t=11500`. At 100 ticks/s:

| phase | wall time |
|---|---|
| boot → compositor → `PRADYOS_FOCUS` → `'k'` armed | **~65 s** |
| left for four advances | **~55 s** |
| three advances actually taken | 39.4 s (10.5 + 10.9 + 18.0) |
| a fourth would have needed | ~11–18 s more → ~51–57 s |

It missed by seconds. **The dominant cost is boot-and-arm at ~65 s of a 120 s
window, not the animation** — which is exactly what the first draft got wrong by
reasoning from the per-advance numbers alone without dividing up the window.
(§0.7's lesson in a new guise: a per-event metric without the total is as
partial as a total without a denominator.)

### What ships

Both, because they address different terms:

1. **Short transition under the test knob** — keep it. It cuts ~10 renders per
   advance and measurably lowers the period. The gate still observes a real
   animated transition, `g_settled` still lands on the final frame (DDR-716),
   and `PRADYOS_PRETRANSITION` / `PRADYOS_CADENCE_OK` still print; only the
   frame *count* changes, and only under the knob. Test mode is derived from
   `g_cadence_ns` already being small rather than a new flag — no new writable
   global (DDR-826). The knob is used by `smoke-cadence` alone, so no other
   gate's behaviour changes.
2. **`timeout 120` → `180`** — this is the one that addresses the actual
   budget. With ~65 s of boot-and-arm and a worst-measured ~9.9 s period, four
   advances need ~40 s against ~115 s remaining: a ~2.9× margin instead of
   missing by seconds.

Requiring fewer than 4 advances is still rejected: a *full* cycle through all
four ambiances is the thing under test (DDR-726).

## 4. What would refute this

If a `smoke-cadence` red still shows `n < 4` after this, the animation was not
the binding cost and the remaining budget must be measured rather than assumed.
If it shows `n = 4` but no `PRADYOS_CADENCE_OK`, the defect is in the sentinel
path, not the timing, and belongs in its own DDR.
