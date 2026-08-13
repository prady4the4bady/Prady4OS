= DDR-915 — the DAG rendezvous must observe its peer, not count iterations

**Status:** ACCEPTED — governs the `smoke-actiondag` fix.
**Date:** 2026-08-13
**Lineage:** DDR-839 (DAG action queue) + DDR-910 (observe, don't assume) →
**DDR-915 (this)**.

## The defect

`smoke-actiondag` fails intermittently with:

```
ACTIONDAG FAIL: agent never published the action ids rc=0
```

Both roles spawn. The sovereign sets MANUAL mode, writes `DAG_GO`, then waits
**400 fixed iterations** for `DAG_IDS`. The agent waits **400 fixed iterations**
for `DAG_GO`, then submits five actions and publishes the ids. Neither budget is
derived from anything; both are bare counts.

## Evidence — the outcome tracks HOST LOAD, not source

Six freshness-verified runs, same probe set, artifacts under `build/artifacts/`:

| artifact | agent wait | result |
|---|---|---|
| `actiondag-20260812T132801Z.log` | `spin_delay` (original) | **FAIL** |
| `dagdiag-20260812T133746Z.log` | + breadcrumbs | **FAIL** |
| `dagdiag-20260812T134230Z.log` | + read rc | **FAIL** |
| `dagdiag-20260812T134705Z.log` | yield-only, no burn | **FAIL** |
| `dagdiag-20260813T022849Z.log` | yield-only | **PASS** (`spins=10`) |
| `dagdiag-20260813T023206Z.log` | `spin_delay` restored (arm B) | **PASS** (`spins=11`) |

Arms A and B differ in exactly the thing a CPU-starvation theory predicts should
matter, and **both pass**. Starvation is refuted. What separates the failing runs
from the passing ones is not the source at all: every failure ran during
back-to-back host builds, every pass ran after an idle gap. The agent needs
~10-11 poll iterations to be scheduled through its rendezvous; under host load
the sovereign consumes its 400 before the agent consumes its 10.

A hypothesis this DDR explicitly records as **refuted**, so it is not re-tried:
fair-share deprioritisation of the busy-waiting agent (arm B disproves it).

## Decision

**Delete both internal iteration caps. The harness `TIMEOUT_S` is the single
authority on how long this gate may wait.**

Raising 400 to some larger number is forbidden — it is an unmeasured constant
either way, and a bigger wrong number still fails on a slower runner. The gate
already HAS a measured, documented, CI-tuned bound: `TIMEOUT_S=120` in
`Makefile:2144`. A second, smaller, arbitrary budget nested inside the test is
the bug, not a safety net.

Each side therefore polls until its key appears, emitting a progress line every
64 iterations. If a peer genuinely never publishes, the gate still fails — by
harness timeout — and the log names which side was waiting and how long, so the
failure stays fully diagnosable. Diagnosability was the only thing the internal
cap bought, and it is preserved without the false negative.

## ROOT CAUSE — the agent was being KILLED, not starved (added after the above)

Removing the caps did its job: it turned an intermittent failure into a
deterministic one, and that exposed the real cause. With no cap the sovereign
spun **12,800+ iterations** (32x the old budget) while the agent emitted **zero**
progress lines — not one per-64 breadcrumb. A slow thread prints; a dead one
does not.

**The agent was killed by the ADR-026 D7 syscall rate limiter.**
`kernel/syscall/syscall.c` kills — not throttles — any agent exceeding
`AETHER_RATE_MAX` (60) counted syscalls per 1 s window, via `sched_exit(137)`,
which never returns to the handler. The rendezvous cost 2 counted syscalls per
poll (`SYS_MEMORY_READ` + `SYS_YIELD`), so the agent died at ~30 iterations
**regardless of whether its peer was cooperating**. The sovereign was untouched
only because `is_agent` is false for it — which is exactly the >160x asymmetry
measured between two structurally identical loops.

Confirmations: the kill code path, and `AGENT_RATE_LIMITED PID=29` in
`build/artifacts/dagdiag-20260813T024223Z.log` (the other such line, `PID=2742943744`,
is the synthetic TCB from `aether_sectest`).

This also retroactively explains the load dependence tabulated above: the two
passing runs finished in `spins=10` and `spins=11` — just under budget. It was
never about scheduling fairness, only about how many polls were needed before
crossing 60 syscalls. **The starvation theory remains refuted; the table above
stands, and its conclusion — that the fixed 400-iteration budgets had to go —
also stands, since they were what hid this.**

**Fix:** ADR-036 supersedes D7's *counting scope* so `SYS_YIELD` is free (a
yielding agent is cooperating, not abusing), and this test paces its polls from
the kernel's own constants — one counted syscall per poll at half the budget,
timed off the zero-syscall vDSO clock rather than an arbitrary delay. See
`user/actiondagtest.c` and ADR-036's two-arm verification bar.

## Why this cannot mask a real defect

The gate's assertions are unchanged: arms 1-5 still run, `-EAGAIN`/`-ESRCH`
ordering is still enforced, `ACTIONDAG FAIL` is still the forbidden sentinel. A
broken queue fails exactly as before. What changes is only that a *slow* peer no
longer reads as a *broken* one.

## Serial interleaving — noted, NOT the cause

`fail()` emits its text in four separate `SYS_WRITE` calls, so another thread's
`kputs` can split it mid-message; this is visible in
`dagdiag-20260812T133208Z.log` (`ACTIONDAG FAIL: ` split by an unrelated
`[user] ELF loaded` line). That is a real serial-atomicity wart, but it is a
*reporting* artifact only — it never changed a pass into a fail, and the
sentinel matching is line-oriented on the leading token. It is recorded here as a
separate, lower-priority concern and is deliberately NOT fixed in this change:
its fix is a locking/buffering correction on the print path, which has nothing to
do with the rendezvous logic and must not be bundled with it.
