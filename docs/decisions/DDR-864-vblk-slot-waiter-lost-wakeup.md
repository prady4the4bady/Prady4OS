# DDR-864 — `virtio_blk` loses a wakeup when two submitters wait for a slot

**Status:** Open defect, documented — **not yet fixed**
**Date:** 2026-08-07
**Scope:** `kernel/drivers/blk/virtio_blk.c`. Found while investigating item 47
(the `-smp 4` flake, DDR-863).

## The defect

`struct vblk` holds **one** waiter pointer:

```c
struct tcb *slot_waiter;   /* a submitter waiting for a free slot */
```

`submit()` registers into it when all `VBLK_NREQ` (8) slots are in flight:

```c
v->slot_waiter = current_thread;
sched_block_on(&v->compl_lock);
```

If a **second** submitter also finds every slot busy, it overwrites the first's
registration. The first thread is now blocked with **no record of it anywhere**.
The release path wakes only the single stored pointer:

```c
if (v->slot_waiter) {
    struct tcb *w = v->slot_waiter;
    v->slot_waiter = 0;
    sched_unblock(w);
}
```

So thread A blocks forever. This is a lost wakeup, and it needs **≥9 concurrent
block requests** to trigger — reachable only with several CPUs issuing I/O at
once, which is exactly the `-smp 4` condition the flaking gates run under.

The surrounding code is otherwise careful: per-request `waiter` pointers, all
slot and vq state under `compl_lock`, and the documented locks-4 pattern where
waiters publish BLOCKED under the same lock a completion takes. The single
`slot_waiter` is the one place that pattern is not followed — the per-request
path has one waiter slot *per request*, which is correct, while the
slot-starvation path has one waiter slot *per device*, which is not.

## Why it is documented rather than fixed here

**It is not proven to be the flake cause**, and saying otherwise would be the
convenient conclusion rather than the supported one. The observed symptom is a
*wrong read* — `[fs] mounted fat32` succeeds, then `/HELLO.TXT not found` and
`[sfs] created 0, verified 0` — which reads as data returning empty or stale,
not as a thread hanging. A lost wakeup produces a hang, and the gate would fail
on timeout with the watchdog (DDR-776) naming the stuck request. That is not
what the logs show.

So there are two possibilities, and the evidence does not separate them:

1. This defect is real but distinct from the flake, and both live in the same
   subsystem.
2. The hang manifests indirectly — a stuck submitter starving the filesystem's
   own retry path — and the FS reports failure rather than blocking.

**And the fix is not safe to make blind.** A correct fix needs an unbounded wait
list, because threads are `kmalloc`-allocated with no fixed table to size an
array against. The options are an intrusive `next` pointer in `struct tcb`
(which the project's own notes flag as needing explicit initialisation in
`sched_create`, or the field is garbage) or a bounded array with a defined
overflow behaviour. Both are real design decisions in the concurrency path of
the subsystem *currently under investigation for intermittent failures*, and
neither can be verified without the QEMU gate cycle.

Changing block-layer concurrency on a hypothesis, unverified, in the middle of a
flake investigation, would make the next red impossible to attribute — was it
the old flake, or the new fix? That is a worse position than the documented bug.

## What would settle it

The item 47 campaign (DDR-863): one pinned SHA, N runs of the five flaking gates
**individually**. If the flake reproduces with the watchdog naming a stuck
request, this defect is implicated and the fix is justified and testable against
a known rate. If it reproduces as wrong data with no stuck request, this defect
is real but separate, and the flake hunt continues elsewhere — most likely in
`virtq_pop_used`/`head2slot` reuse or the descriptor free-chain.

Either way the fix lands **after** there is a measured baseline to compare
against, not before.

## Recorded so it is not lost

This is a genuine defect regardless of the flake. It is filed here rather than
left in a session transcript so that the next person to touch
`virtio_blk.c:submit` sees it, and so the item 47 campaign has a named
hypothesis to test rather than starting from nothing.
