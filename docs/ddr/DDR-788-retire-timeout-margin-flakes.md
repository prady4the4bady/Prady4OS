# DDR-788 — retire the DDR-783 flake class: margin is now free for eligible gates

**Status:** implemented — 7 gates raised to `TIMEOUT_S=120`, verified free on
success (measured 3–34 s each, all PASS under the new ceiling). Infrastructure; follows **DDR-783** (which
fixed one gate) and **DDR-785** (which made this fix free).

## Why now

DDR-783 raised `smoke-fs` from the harness default 30 s because its last required
sentinel lands at **t=24.26 s** — 19 % margin, which flaked on a slower CI runner.
That fixed *one* gate. The underlying condition — "a gate asserts a late sentinel
under a tight window" — was left alive everywhere else, because before DDR-785 the
timeout **was** the runtime and generosity cost real wall-clock.

DDR-785 changed that: an eligible gate (no `FORBIDDEN_SENTINEL`) now stops the
guest as soon as its sentinels appear, so **its timeout no longer affects a
passing run at all**. Raising margin is now free on success.

## Evidence — measured, and smaller than assumed

Counting `boot_test.sh` invocations in the Makefile:

| | gates |
|---|---|
| total | 92 |
| declare `FORBIDDEN_SENTINEL` (timeout still *is* the runtime — untouchable) | 38 |
| eligible, already have an explicit `TIMEOUT_S` | 43 |
| **eligible and still on the default 30 s** | **11** |

Only 11 gates are in scope — worth checking before assuming a sweeping change was
needed.

**One gate is demonstrably in the DDR-783 danger zone, not two.** The first draft
of this DDR asserted that both `smoke-fs-sfs-rw` and `smoke-fs-rw` were at risk,
reasoning from which sentinels they name. Measuring instead of reasoning
corrected it:

| gate | measured wall-clock | old window | verdict |
|---|---|---|---|
| `smoke-fs-sfs-rw` | **30 s** | 30 s | **at the margin** — asserts `journal abort/commit/replay OK` / `version-isolation OK` / `compress/readback/tag OK`, the same chain DDR-783 timed at 24.09–24.26 s |
| `smoke-fs-rw` | 5 s | 30 s | **not at risk** — its sentinels land early despite sitting in the same self-test |
| `smoke-fs-ext4` | 34 s | 30 s | includes its ext4 image build, so not directly comparable — but nowhere near 120 s |
| `smoke-uaccess` / `smoke-cowfork` / `smoke-mitigations` | 4 s / 3 s / 4 s | 30 s | ample margin |

So the case rests on `smoke-fs-sfs-rw`, which sat with essentially **zero**
margin — the identical condition that took `smoke-fs` down in run 30192189559 —
plus the fact that margin is now free for the rest.

## Decision

Set `TIMEOUT_S=120` on the eligible default-30 gates.

- **Cost on success: zero.** Early exit stops the guest when the sentinels appear,
  so a gate that finishes at 24 s still takes 24 s whether the ceiling is 30 or
  120.
- **Cost on failure: bounded and real** — a genuinely failing eligible gate now
  burns 120 s instead of 30 s before reporting. That is the honest trade: rare
  failures get slower so that routine passes stop flaking. Failures are supposed
  to be rare; flakes were not.
- **The 38 `FORBIDDEN_SENTINEL` gates are not touched.** For them the timeout is
  still the runtime (DDR-785 deliberately excludes them), so raising it would add
  90 s each to every green run — 57 minutes of pure waiting. Explicitly out of
  scope.

One of the eleven is justified by measurement; six more are raised with it on the
strength of "free on success" rather than per-gate timing — a deliberate choice to
retire the *class* rather than play whack-a-mole one gate per incident, which is
the DDR-783 pattern. Seven gates changed in total.

**Three are deliberately left on the default**, because raising them would be
noise: `smoke` (two invocations) asserts only `NEXUS KERNEL OK`, measured at
**t=0.31 s**, and `smoke-mkfs-sfs` is a host-side tool gate with no boot sentinel.

## Gate

No new gate. Verified by the existing suite continuing to pass **and by wall-clock
not regressing** — the "free on success" claim is falsifiable and was checked:

```
smoke-uaccess    4 s     smoke-cowfork     3 s     smoke-mitigations  4 s
smoke-fs-rw      5 s     smoke-fs-sfs-rw  30 s     smoke-fs-ext4     34 s
```

all under the new 120 s ceiling, all PASS (and all against the DDR-787 kernel).
If total CI wall-clock regresses against run 30200918063 (105.8 min), the claim is
wrong and this must be revisited.

## Architecture prerequisite checklist

- Kernel code, syscalls/NSI, TCB, PMM/VMM, capabilities, AETHER, scheduler hooks,
  FS/on-disk format, compositor: **none touched**. Makefile timeouts only.
- Gate count unchanged: **106**.
- **Security invariants:** none engaged. No kernel or user code. Explicitly not an
  S2 matter — `timeout` still bounds every gate, and a hung kernel still fails
  (with no sentinel) exactly as before; only the deadline moves.

## Non-goals

Touching the `FORBIDDEN_SENTINEL` gates, extending early exit to them (that needs
its own evidence — see DDR-785's non-goals), and re-tuning any gate that already
carries an explicit `TIMEOUT_S`.
