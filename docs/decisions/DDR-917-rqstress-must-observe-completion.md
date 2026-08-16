= DDR-917 — rqstress must observe completion, not assume it inside a fixed window

**Status:** ACCEPTED — fixes the intermittent `[smp] rqstress FAIL` that reddens
multiple CI shards.
**Date:** 2026-08-14
**Lineage:** DDR-SMP-rq-1 (the proof) + DDR-910 (observe, don't assume) +
DDR-785 (a foreign probe FAIL fails the gate that is booting).

## The defect

`rqstress_proof()` (kernel/main.c:572) spawns 3 waves x 8 `rqstress_worker`
threads and asserts all 24 completed:

```c
for (int wave = 0; wave < 3; wave++) {
    for (int i = 0; i < 8; i++) sched_create(rqstress_worker, 0, "rqs");
    smp_resched_all();
    uint64_t dl = g_ticks + 100;                 /* fixed 1 s, 100 Hz PIT */
    while (g_rqs_done < (uint32_t)((wave + 1) * 8) && g_ticks < dl)
        yield();
}
kputs(g_rqs_done == 24 ? "[smp] rqstress OK\r\n" : "[smp] rqstress FAIL\r\n");
```

Each wave's wait is bounded by a fixed 100-tick deadline and then **falls
through unconditionally**. When the last wave's deadline expires with workers
still runnable, `g_rqs_done == 24` is evaluated immediately and reports FAIL for
threads that were **late, not lost**. Nothing re-checks afterwards.

This is the DDR-910 defect shape exactly: the proof asserts on a *timer* instead
of on the *outcome*.

## Why it hurts far more than one gate

Per DDR-785 the boot harness fails ANY gate whose boot contains a foreign probe
FAIL. `rqstress_proof` runs during `fs_test_thread`, i.e. on every boot, so a
single late wave reddens whatever gate happens to be running. Observed in CI run
31803482520 (a **docs-only** commit):

```
[smoke] FAIL — a probe reported 'rqstress FAIL' during this gate's boot.
[smp] rqstress FAIL
make: *** [Makefile:2302: smoke-sfs-gc] Error 1
shard 3: FAILED at smoke-sfs-gc after 9 of 21 gates
```

Shards 2, 3 and 5 all failed that run; `smoke-sfs-gc` has nothing to do with
rqstress. That is why docs-only commits go red while code commits pass — the
signal is pure timing, uncorrelated with content.

## Decision

1. **Add a final drain that observes the outcome.** After the wave loop, wait
   for all 24 to land, bounded. The per-wave deadlines stay as *pacing* (they
   keep the waves overlapping, which is the point of the stress), but they no
   longer decide the verdict.
2. **Report the observed count on failure.** `[smp] rqstress FAIL n=<count>` —
   a bare FAIL is undiagnosable, and this proof previously gave no way to tell
   "23 of 24 landed late" from "the runqueues genuinely lost threads".

### The bound is derived, not chosen

The three waves already budget 100 ticks each = 300 ticks total. The drain is
given **the same 300-tick budget the waves collectively had**, so the proof's
worst case is at most 600 ticks (6 s at the 100 Hz PIT) — unchanged in order of
magnitude and far inside every consuming gate's `TIMEOUT_S` (the tightest is
120 s). S2 holds: the wait is still bounded and still terminates.

## What this does NOT do

It does not weaken the assertion. `g_rqs_done == 24` is still required — every
one of the 24 threads must complete. The change is only that the proof now waits
for the answer before reading it. A genuine runqueue defect that loses a thread
still fails, and now fails with the count attached.

## Explicitly not the cause

- Not sfs.c: it has no global mutable state to race on (per DDR-880) and
  `smoke-sfs-gc` was collateral, not the failing subject.
- Not the "lost fs_test_thread" signature of DDR-880/item 47: all boot-stamps
  print in these runs, and the probe reaches its own kputs to report FAIL.
