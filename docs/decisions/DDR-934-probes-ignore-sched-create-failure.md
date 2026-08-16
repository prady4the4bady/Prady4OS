= DDR-934 — the blk probes ignore `sched_create` failure; and the missing-IPI theory is refuted

**Status:** ACCEPTED. **Corrects DDR-932's proposed mechanism.**
**Date:** 2026-08-16
**Lineage:** DDR-930 (done=0x0) → DDR-932 (missing wake IPI) → **DDR-934 (this)**.

## First: DDR-932's mechanism does NOT hold

DDR-932 observed a real correlation — the two probes without
`smp_resched_all()` are exactly the two reporting `done=0x0` — and proposed
that newly created threads sit unpicked because nothing wakes an idle CPU.

**Refuted by the idle loop** (`kernel/proc/sched.c:725-734`):

```c
__atomic_store_n(&pc->idle, 1, __ATOMIC_SEQ_CST);
if (rq_has_ready()) { …; continue; }      /* work appeared */
__asm__ volatile("sti; hlt");             /* sleep until wake IPI / timer */
```

Interrupts are enabled **before** the halt, and `rq_has_ready()` is checked
first under SEQ_CST ordering that pairs with the waker's `rq_push`. So an idle
AP wakes on **its own LAPIC timer** — within one tick, ~10 ms — and steals.

`smp_blk_integrity` allows 400 ticks plus a 400-tick drain (DDR-918) = **800
ticks ≈ 8 s**. A missing wake IPI costs at most one tick. It cannot produce
8 seconds of nothing. The correlation is real; the proposed mechanism is not
sufficient, and implementing it as *the fix* would have been fixing on a theory
the code already contradicts.

## The mechanism that does fit

`sched_create_state` returns **NULL** on allocation failure — for the TCB, or
for the 16 KiB kernel stack:

```c
struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
if (!t) return 0;
uint64_t base = (uint64_t)(uintptr_t)kmalloc(STACK_SIZE);
if (!base) { …; return 0; }
```

Both probes **discard that return value**:

```c
sched_create(blkint_worker, (void *)0, "bi0");   /* return ignored */
sched_create(blkmq_reader,  (void *)0, "mq0");   /* return ignored */
```

If creation fails, no thread exists. Nothing runs, nothing sets a bit, and the
probe waits out its full budget and reports `done=0x0`. That single fact
explains every observation at once:

| observation | explained? |
|---|---|
| `done=0x0` — no worker returned | yes: none exist |
| no `[vblk] stuck` from the watchdog | yes: no requests were ever submitted |
| `[hb]` present, `g_ticks` advancing | yes: the system is healthy |
| 8 s of nothing | yes: there is nothing to schedule |
| intermittent across runs | yes: heap pressure varies by boot |

Four workers cost 4 x (sizeof(struct tcb) + 16 KiB) ≈ 64 KiB of kernel heap, and
these probes run late in boot after many other probes have allocated.

**This is a hypothesis with a strong fit, not yet a confirmed cause** — the
probes currently cannot tell "never created" from "created but never ran".

## Decision

Check the return value and report it. `smp_blk_integrity` and `blkmq_proof` now
count successful spawns and print `spawned=<n>/<total>` on failure, alongside
DDR-930's `prog=`:

```
[smp] blk integrity FAIL workers-late done=<hex> spawned=<n>/4 prog=<i0..i3>
[blk] multi-inflight FAIL done=<hex> spawned=<n>/2
```

Reading it:

- `spawned<total` ⇒ **allocation failure**. The defect is heap exhaustion at
  that point in boot, not scheduling — and the fix is in allocation/lifetime,
  not the scheduler.
- `spawned=total` with `prog=0,0,0,0` ⇒ the threads exist and never ran ⇒
  genuinely scheduling, and DDR-932's IPI question becomes live again (though
  the idle-loop analysis above still argues against it).
- `spawned=total` with `prog` advanced ⇒ they ran and stalled mid-loop.

Three different defects, currently indistinguishable, each pointing at a
different subsystem. This is the same "one message, several causes" class as
DDR-917/918/920/923.

## Not done

`smp_resched_all()` is **not** added to the two probes in this slice. It would
be harmless and matches `rqstress_proof`, but adding it now would confound the
measurement: if the next run passes, we could not tell whether the IPI helped or
the heap simply had room. Add it only after `spawned=` has spoken.
