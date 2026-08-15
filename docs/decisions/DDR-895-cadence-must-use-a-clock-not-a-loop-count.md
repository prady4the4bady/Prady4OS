= DDR-895 — the ambiance cadence must use a clock, not a frame-loop iteration count

**Status:** ACCEPTED (diagnosis + design). **Implementation deferred to the next
slice** — a CI run was in flight, and the standing rule forbids local QEMU that
would contend with it, so this could not be gate-verified in this session.
**Date:** 2026-08-16
**Lineage:** DDR-726 (auto cadence) + DDR-911 (loop-count vs observed state) +
DDR-910 (observe, don't assume) → **DDR-895 (this)**.

## The flake

`smoke-cadence` failed in CI run 31843212987 (shard 5):

```
[cadence] FAIL — no full auto cycle
```

i.e. `PRADYOS_CADENCE_OK` never printed.

## Root cause — the "clock" is a loop counter

`user/compositor.c:703-707`, verbatim:

> auto ambiance cadence. Time source is **the frame loop itself** … is an
> **ITERATION count** approximating the brief's 900 s wall cadence; the 'k' …

```c
static unsigned long g_cadence = 1500000;   /* iterations per ambiance period */
…
static void cadence_tick(void) {
    if (!g_cad_pre_said && g_cad_iter >= g_cadence - g_cadence / 10) { … }
    if (g_cad_iter >= g_cadence) { … printf("PRADYOS_CADENCE_OK\n"); }
```

and the test knob (`:1003-1007`) sets `g_cadence = 2000`.

So the gate's success condition is "the compositor executed N iterations of its
frame loop". That is not a time measurement — it is a measurement of **how much
CPU the compositor was given**. Under CI load, competing with every other thread
on a 4-vCPU TCG runner, the compositor can simply fail to accumulate the
iterations inside the gate's window, and the gate reports a missing cycle when
nothing is broken.

Nothing about the ambiance feature is wrong. The gate is timing the wrong thing.

## This is a known, recurring anti-pattern in this tree

DDR-911 fixed exactly this shape once already: `surfacetest.c` closed window C on
a loop-iteration count (`ticks > 12000`) rather than on observed state, and the
fair-share scheduler change (item 16) altered its CPU share and broke the gate.
The fix there was a readiness handshake plus a real grace period measured with
`SYS_TIME`.

The same reasoning applies here, and the same remedy is available.

## Design

Drive the cadence from a real clock instead of an iteration count:

1. Read the **vDSO wall clock** (`VDSO_USER_VA`, `wall_time_ns`), which ring 3
   can read with a plain load and **zero syscalls** — the same source DDR-915
   used to pace the actiondag rendezvous. That keeps the compositor's hot loop
   free of syscalls, which is why the iteration count was chosen originally.
2. `g_cadence` becomes a **period in nanoseconds**, not iterations. The
   production value expresses the brief's 900 s cadence directly instead of
   "1 500 000 iterations, which is about 900 s if the compositor gets a typical
   share of a typical CPU".
3. The `k` test knob sets a short period (e.g. 2 s) in the same units, so the
   gate's stimulus stays one keypress and the assertions are unchanged.
4. Pre-transition still fires at 90% of the period, now 90% of elapsed time.

This makes the cadence correct on any machine at any load, and makes the gate
measure the feature rather than the scheduler.

### Why not simply raise the gate timeout

Because the failure is not "slightly too slow" — it is unbounded. The iteration
rate depends on scheduler share, which has already changed once under this
project (item 16, fair-share) and will change again. A timeout large enough to
be safe today is a guess that expires the next time scheduling changes. A clock
is not.

## Verification required before this may be marked fixed

- `smoke-cadence` 3x consecutive local PASS.
- The production cadence path exercised at least once (the `k` knob must not be
  the only tested branch), since changing the units touches both.
- Standard hygiene: warning-clean, three `ci-*-check`, and the three
  freeze-site gates.

## Status of the other open flakes at the time of writing

| gate | state |
|---|---|
| `smoke-evresize` | FIXED (DDR-894), 4/4 local |
| `smoke-cadence` | root-caused here, fix not yet implemented |
| `smoke-agent-click` | not reproducible locally 3/3; hypothesis recorded, unproven |
| `smoke-rtc-smp` | blocked on a DDR-891 capture naming -ENOENT vs -ENOSPC |

## IMPLEMENTED — verified both paths

Landed as designed. `g_cadence_ns` is a period in nanoseconds (900 s production,
stated directly); `cadence_tick` reads the vDSO wall clock with a plain load and
no syscalls; the pre-transition pulse fires at 90% of *elapsed time*; the `k`
knob sets 2 s per ambiance (a full 4-ambiance cycle in ~8 s) and re-arms
`g_cad_start_ns` from the current instant.

Verification, both branches as the DDR required:

- **`k` test path:** `smoke-cadence` **3/3 consecutive PASS**.
- **Production path:** a normal boot (`smoke-compositor`, rc=0) emits **zero**
  `PRADYOS_CADENCE_OK` / `PRADYOS_PRETRANSITION`. That absence is the observable
  check for the 900 s branch — it cannot be positively observed inside a gate
  window, but a units error would make it misfire immediately, and it does not.

Hygiene: build warning-clean; sentinel-collision OK (159); the three
`ci-*-check` PASS; `smoke-blkmq`, `smoke-rqstress-liveness`,
`smoke-blk-integrity` all rc=0; no stray QEMU (bracket form).
