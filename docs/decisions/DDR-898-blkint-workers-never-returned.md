= DDR-898 — `done=0x0` means the blk-integrity workers never RETURNED, and they were not stuck in I/O

**Status:** ACCEPTED (diagnosis + instrument). **No driver change** — §6.0-D.
**Date:** 2026-08-16
**Lineage:** DDR-886 (probe disambiguation) → **DDR-898 (this)**.
Related: DDR-775/776 (the unbounded completion wait), DDR-785 (foreign probe FAIL).

## The capture

CI 31907631454, shard 4, `smoke-ftruncate` failed as collateral:

```
[smp] blk integrity FAIL workers-late done=0x0000000000000000
[blk] multi-inflight FAIL done=0x0000000000000000
```

DDR-886's disambiguation did its job: `reason=workers-late`, **not**
`checksum-mismatch`. Per §6.0-D no virtio-blk driver change is authorized, and
DDR-775/776's completion-**loss** (bad data) hypothesis is ruled out here.

## What `done=0x0` actually means — stronger than "late"

`blkint_worker`'s last statement is unconditional:

```c
__atomic_or_fetch(&g_blkint_done, 1u << (ok ? id : id + 8), __ATOMIC_SEQ_CST);
```

Every worker that **returns** sets a bit — success (0..3) *or* error (8..11).
`done=0x0` therefore means **not one of the four workers returned**. They did not
finish slowly; they did not finish at all.

The label `workers-late` (which I wrote in DDR-886) is imprecise for this case
and should be read as "no verdict from the workers", not "the workers were a bit
slow".

## Where they were NOT: blocked in block I/O

The obvious candidate was `bd->read`, the only blocking call in the worker, which
reaches the **unbounded** wait at `virtio_blk.c:259-260`:

```c
while (!v->req[s].done)
    sched_block_on(&v->compl_lock);
```

A worker parked there never returns — a perfect fit for `done=0x0`. **Refuted by
two facts in the same log:**

1. **Zero `[vblk] stuck` lines.** `virtio_blk_watchdog` (`VBLK_STUCK_TICKS 500`,
   ~5 s) prints `[vblk] stuck dev= slot= lba= age=` for any request that is
   `used && !done` past that age. Not one appeared, so no worker held an
   outstanding request for 5 s.
2. **`[hb]` heartbeats are present**, so `g_ticks` was advancing and the
   watchdog (driven from `timer_tick` every 100 ticks) was genuinely running —
   its silence is informative, not an artifact of a frozen timer.

So the workers were not waiting on block I/O.

## What remains

The workers were not scheduled, or made so little progress they never reached a
single completed iteration — starvation, not I/O. That is consistent with the
probe's own budget: `smp_blk_integrity` waits 400 ticks then drains another 400
(DDR-886), i.e. ~8 s, during which four threads on a 4-CPU system produced
nothing at all.

**This DDR does not claim which.** `g_blkint_done` cannot distinguish "never
scheduled" from "ran and stalled mid-loop", because it is only written at the
very end of the worker.

## Instrument

Add a per-worker progress counter, published in the failure line:

```
[smp] blk integrity FAIL workers-late done=<hex> prog=<i0>,<i1>,<i2>,<i3>
```

where each `i` is that worker's completed read iterations (of 64). Reading it:

- `prog=0,0,0,0` ⇒ the workers never ran a single iteration ⇒ scheduling /
  spawn, and the next question is `sched_create`'s placement and the runqueue
  the workers landed on.
- `prog` partially advanced ⇒ they ran and stalled; the value names how far, and
  the stall is then localisable to that iteration's `bd->read`.

Costs one relaxed atomic increment per read iteration on a probe path.

## Explicitly not done

No change to `virtio_blk.c`. §6.0-D stands, and the S2 unbounded wait — while a
real defect on its own merit (DDR-775/776) — is **not** implicated by this
capture, since the watchdog proves no request was outstanding.

## CONVERGENCE — a second, independent gate shows the same shape

CI 31911253495 shard 3 (`smoke-agent-click`, with DDR-896's widened dump):

```
PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82   <- last agent line, no AGENT_START
```

The clicked agent gets a pid and never prints `PRADYOS_AGENT_START` — the first
statement of its `main()`. It never executed one instruction of user code.

Put beside this DDR's finding — four blk-integrity workers, `done=0x0`, no
worker returned, and the watchdog proving none was waiting on I/O — **two
unrelated subsystems now show freshly created threads that never run.**

That makes "the blk workers were merely slow" the less likely reading and
promotes a common cause in thread creation/scheduling. Candidates to examine, in
order, none yet tested:

1. `sched_create`'s placement — which runqueue does a new thread land on, and
   can that CPU be one that is not picking up work?
2. The DDR-892 fallback: when `switch_wait_offcpu_sched` hits its bound the CPU
   requeues `next` and runs **idle**. If a CPU repeatedly takes that path it
   makes no progress on real work. Note new threads have `on_cpu = -1` at
   creation (`sched.c:655`), so they should *not* trigger the wait — this needs
   checking rather than assuming.
3. `smp_resched_all()` delivery — whether the IPI that should wake an idle CPU
   to pick up a newly enqueued thread is actually landing.

The `prog=` instrument added by this DDR discriminates (1)/(2) for the blk case:
`prog=0,0,0,0` means never scheduled; anything non-zero means scheduled then
stalled.
