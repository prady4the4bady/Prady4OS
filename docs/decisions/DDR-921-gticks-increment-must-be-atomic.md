= DDR-921 — `g_ticks++` is a non-atomic RMW and loses ticks

**Status:** ACCEPTED.
**Date:** 2026-08-15
**Separate from DDR-919** — that fixed the *freeze* (interrupts masked while
spinning off-CPU). This is a distinct defect the fix made *visible*: with
interrupts enabled more often, concurrent entry into `timer_tick` is now
observable. Deliberately not folded into DDR-919.

## Evidence — measured, not argued

CI run 31843212987 (the run carrying the DDR-919 fix). Of **12** `[hb]` lines in
the failing shard's serial dump, **two values print twice**:

```
2  [hb] t=7500
2  [hb] t=11000
```

A log-duplication artifact would repeat contiguous blocks, not two scattered
values out of twelve. So `timer_tick` genuinely executed twice while `g_ticks`
held the same value.

## Why that is a lost tick, not a cosmetic double-print

`kernel/irq.c:5` — `volatile uint64_t g_ticks = 0;`
`kernel/idt.c:139` — `g_ticks++;`

`volatile` orders the access; it does **not** make the read-modify-write atomic.
Two CPUs entering `timer_tick` concurrently can both read N, both write N+1, and
both observe `(g_ticks % 500) == 0` — printing the same value twice **and losing
one tick**. Every consequence follows from the lost increment, not the duplicate
print:

- `g_ticks`-relative deadlines run long by however many ticks were lost.
- The vDSO wall clock advances in the same block
  (`wall_time_ns += 10000000`), so ring-3 time drifts slow.
- `(g_ticks % 100) == 0` (blk watchdog) and `% 10` (lwIP) can be **skipped
  entirely** when the value they would have matched is the one that was lost.

## Why concurrent entry is possible at all, given the BSP-only design

`idt.c:177-187` routes the LAPIC timer so that only the BSP runs `timer_tick`
(APs call `sched_tick` for preemption only). That intent is correct, but it is
not airtight:

- The dispatch is `if (pc && !pc->is_bsp) … else timer_tick(r);` — when
  `this_cpu()` returns **NULL** (an AP before its per-CPU area is live) the
  `else` branch runs `timer_tick` on that AP.
- `idt.c:200-202` also calls `timer_tick` from the **PIT** (IRQ0) path with no
  BSP check. The PIT is masked when the LAPIC takes over (`lapic.c:156`), but
  the handover is a window, not an instant.

This DDR does **not** claim which of those produced the observed duplicates.
It fixes the increment so that neither can lose a tick, which is correct
regardless of which path is responsible — and cheap enough that narrowing it
first would be the more expensive order.

## Decision

Make the increment a single atomic read-modify-write:

```c
__atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
```

`RELAXED` is sufficient and correct: `g_ticks` is a counter, not a lock or a
publication flag. Nothing in the tree infers ordering of *other* memory from a
`g_ticks` value — the deadline sites all do plain comparisons — so no acquire or
release edge is needed, and demanding one would cost a barrier on every tick for
no invariant.

A `_Static_assert` pins the type at 8 bytes so the operation stays lock-free if
the declaration is ever changed.

## What this does NOT do

It does not narrow which path caused the concurrent entry (pre-percpu AP window
vs PIT/LAPIC handover). Both remain worth closing on their own merit and are
recorded here as follow-on; the atomic increment makes neither of them able to
lose a tick in the meantime.

## Gate

`smoke-blkmq` must stay rc=0, plus the standard three `ci-*-check` and a
warning-clean build. No new gate: this changes the correctness of an existing
counter, and the existing timing-sensitive gates are its regression surface.
