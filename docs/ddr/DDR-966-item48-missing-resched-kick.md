# DDR-966 — Item 48: the blk proofs spawn workers and never kick the APs

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## 1. A correction to §0.2's stated root cause

`CLAUDE.md` §0.2 and §6.1 record Item 48's confirmed root cause as
*"`sched_create` NULL return under heap pressure (DDR-934) — add NULL-check +
KASSERT"*. **The captures no longer support that**, and the reason is that
DDR-934's own instrument did its job.

DDR-934 added the spawn counter precisely so *"never created"* could be told
from *"created but never ran"* — both of which surface as `done=0x0`. Every
capture since reports:

```text
[blk] multi-inflight FAIL done=0x0000000000000000 spawned=2/2
```

`spawned=2/2` means **both `sched_create` calls succeeded**. The NULL-return
hypothesis is therefore refuted by the very counter added to test it: the
threads exist. This is the diagnostic working as designed, and §0.2's
attribution is now the stale half of it.

The NULL checks stay — they are correct and they are what makes this
discrimination possible — but they are not the fix, and no `KASSERT` is added:
asserting on a condition the data says does not occur would trade a diagnosable
FAIL line for a panic, and buy nothing.

## 2. What the surviving reading is

§0.2's other half stands: `reason=workers-late` is *"a scheduling issue, NOT a
driver bug"*. With the threads proven created, "created but never ran" is the
whole of it.

Compare the three proofs that spawn kernel workers and then wait:

| proof | spawns | kicks the APs? |
|---|---|---|
| `rqstress_proof` (`main.c:671`) | 8 × 3 waves | **yes** — `smp_resched_all()` after each wave |
| `blkmq_proof` (`main.c:735`) | 2 | **no** |
| `smp_blk_integrity` (`main.c:803`) | 4 | **no** |

`sched_create` enqueues onto the **calling** CPU's run queue (`rq_push` on
`this_cpu()`), so a freshly created worker is only picked up by another CPU when
that CPU next runs its scheduler. An idle AP sits in `hlt`; absent an IPI it
wakes on its own timer tick. `smp_resched_all` (`smp.c:144`) is exactly that
kick — it sends `LAPIC_WAKE_VECTOR` to every present non-BSP CPU.

Meanwhile the BSP spins in `while (… && g_ticks < dl) yield()`. So the workers
are runnable, the BSP is burning its deadline, and the APs are halted until a
timer tick happens to collect them. That is `done=0x0` with `spawned=2/2`, and
it is why the failure is intermittent and only under `-smp 4`.

## 3. Decision

Add `smp_resched_all()` immediately after the worker spawns in `blkmq_proof` and
`smp_blk_integrity`, matching the `rqstress_proof` pattern §6.1 names.

This is not a virtio-blk change, which §0.2 and §6.0-D forbid without
`reason=checksum-mismatch`. It touches neither the driver nor the block layer —
only the order in which two probes wake the CPUs that are supposed to run their
workers.

## 4. Why this is not "a fix on hypothesis" (§6.0-B)

§6.0-B requires the failing run's instrument output before a fix. It exists:
`spawned=2/2` is the instrument output, it was captured repeatedly across
shards 0/3/4/5, and it eliminates the allocation reading outright. The remaining
mechanism is read directly off the code — the two proofs demonstrably omit a
call the third makes — rather than inferred from timing.

## 5. What would refute this

- The signature recurring **with** `smp_resched_all()` in place → the workers
  are being woken and still not running; the defect is in the pick, not the
  wake, and belongs with the DDR-936/947 run-queue work.
- `reason=checksum-mismatch` appearing → a genuine data defect, and per §6.0-D
  *that* is when a virtio-blk driver DDR becomes permitted.

## 6. Honest limit on verification

Item 48 is intermittent in CI and has never reproduced locally on demand. A
local green therefore cannot prove this fix; it can only show no regression.
Confirmation is the absence of the signature across CI over time, and this DDR
should not be read as closing Item 48 until that accumulates.
