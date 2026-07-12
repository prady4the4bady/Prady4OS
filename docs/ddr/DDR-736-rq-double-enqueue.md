# DDR-736 — rq double-enqueue: atomic cross-queue rq_on claim

**Status:** proposed (pre-code)
**Layer:** proc (per-CPU runqueues, DDR-SMP-rq-1/2/3 family).

## Problem — two CI failures, one root cause

Two consecutive CI runs failed in the same early-SMP window with different
symptoms: a `kfree: double free` (run 29185317616, during the SMP proofs) and a
silent hang mid-SFS-create (run 29194307211, the boot thread blocked on virtio
completions and never resumed). Neither reproduces locally under KVM (40+
clean runs); CI's TCG runners widen SMP race windows enormously.

Both trace to one invariant break. `rq_push` claims idempotence "via `rq_on`",
but the check-and-set runs under **the target queue's own leaf lock** — and two
concurrent pushers use two *different* locks:

- **waker:** `sched_unblock` (e.g. a virtio-blk completion IRQ on CPU B) wins
  the `BLOCKED -> READY` CAS in the window between the blocker's
  `state = THREAD_BLOCKED` store and its `context_switch`, then
  `rq_push(B, t)` under `g_rq[B].lock`;
- **blocker:** CPU A's own `schedule()` then observes `prev->state == READY`
  (the waker's CAS) and `rq_push(A, prev)` under `g_rq[A].lock`.

Neither lock orders the other, so both can read `t->rq_on == 0` and both link —
**one tcb into two FIFOs through a single `rq_next` pointer**. That breaks
rq-2's exclusion premise ("a thread sits in exactly ONE queue, so exactly one
CPU pops it") at its root, with two downstream corruptions:

1. The second link overwrites `rq_next`/tail state, mangling the first list —
   threads silently vanish from a ready queue → the **hang** (the blocked FS
   thread was woken, queued, then lost).
2. Both queues pop the "same" tcb → **double-run** → two CPUs execute on one
   kernel stack → heap/stack corruption → the **double free** the KASAN slab
   check caught.

(The sched_block_on comment's "benign spurious wake — schedule() just
re-queues us" was written for the pre-rq-1 ring-walk scheduler, where a wake
was a pure state flip with no queue to double-insert into. rq-1 made the
double-push physical; rq-2/rq-3 widened the racing window; DDR-735's timing
shift + TCG made CI hit it.)

## Decision — the rq_on claim becomes atomic (one queue, ever)

`rq_push` claims membership with `__atomic_exchange_n(&t->rq_on, 1, ACQ_REL)`
**before** touching any queue: the winner links into its queue; the loser
no-ops (the thread is already queued somewhere — exactly the idempotence the
old code intended, now sound across queues). `rq_take`'s unlink clears the flag
with a RELEASE store so the next claimant (possibly on another CPU, not holding
this queue's lock) sees a fully unlinked tcb.

This repairs the invariant for **every** present and future pusher pair, not
just the block/wake race — preemption re-queues, steal transients, and rq-3
wake kicks all funnel through the same claim.

No lock-ordering change: the claim is a lock-free flag; queue locks stay leaf.
The losing pusher's thread is on the *winner's* queue, findable by that CPU's
pick or any steal — semantics identical to the intended single push.

## Gate

No new gate: the race lives under `smoke-rqstress` / `smoke-smpsched` /
`smoke-crosswake` and every blk-heavy boot (it hit twice in CI). Full suite
must stay green; the KASAN double-free diagnostic (previous commit) remains as
the tripwire if any related corruption survives.

## Non-goals

- No revert of DDR-735 (it only surfaced the latent race).
- No global scheduler lock reintroduction — the fix preserves rq-2's lock-free
  hot path exactly; it adds one uncontended atomic exchange per enqueue.
