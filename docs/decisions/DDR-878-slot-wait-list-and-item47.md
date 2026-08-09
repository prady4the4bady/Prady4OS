= DDR-878 — virtio_blk slot wait list fixed; the `-smp 4` flake is NOT it (item 47)

**Status:** Accepted (fix) + Open defect, characterised (flake)
**Date:** 2026-08-09
**Supersedes the open half of:** DDR-864. Refines DDR-863.
**Scope:** `kernel/drivers/blk/virtio_blk.c`, `kernel/proc/sched.{c,h}`.

## Two separate findings, kept separate on purpose

DDR-864 recorded a real defect and named it as the leading hypothesis for the
`-smp 4` flake, while explicitly refusing to fix it blind. This DDR does both
halves it asked for: it fixes the defect, and it **rules the defect out** as the
flake's cause. Those are two results, and merging them would have produced the
convenient conclusion rather than the supported one.

## 1. The fix — an unbounded FIFO instead of one pointer

`struct vblk` held **one** `struct tcb *slot_waiter`. A second submitter finding
all eight request slots busy overwrote the first's registration, leaving the
first thread blocked with no record of it anywhere.

Replaced with an intrusive FIFO (`slot_head`/`slot_tail` + `tcb.blk_wait_next`):

- `blk_wait_next` is **separate from `rq_next`**. A thread waiting for a slot is
  BLOCKED and simultaneously not on any ready queue; reusing `rq_next` would
  splice the two lists together.
- It is initialised in `sched_create` alongside `rq_next`/`rq_on`. `kmalloc`
  does not zero, so an uninitialised link is a garbage pointer the block layer
  later dereferences — this project has been bitten by exactly that before.
- `slot_wake_one()` wakes **one** waiter per freed slot, so the wake count
  equals the resource count. The woken thread re-checks in `submit()`'s loop and
  re-queues if another CPU took the slot first: a spurious wake is safe, a lost
  one is not possible.
- It is called on **both** paths that free a slot. The original woke nobody on
  the `virtq_add` failure path, so descriptor exhaustion could strand every
  waiter even with the single-waiter bug fixed. Same release, same wake.

## 2. The measurement — and why the fix is not the flake fix

**Baseline, one pinned SHA (`304f99f`), each gate run individually:**

| Gate | Failures |
|---|---|
| `smoke-rqstress-liveness` | **1 / 8** |
| `smoke-msixap` | 0 / 8 |
| `smoke-blkmq` | 0 / 8 |
| `smoke-blk-integrity` | 0 / 8 |
| `smoke-sfs-btree-smp4` | 0 / 8 |

This already corrects DDR-863. The flake is **not** a broad `-smp 4` problem:
the four block-layer gates were clean 32/32. It concentrates in one gate.

**The precondition witness.** `submit()` now prints `[vblk] slot wait list
depth>=2` the first time a second submitter queues — the exact case the old
single pointer overwrote. In an instrumented `-smp 4` boot it fires **zero
times**. The old bug's precondition does not occur in this workload, so the old
bug cannot be causing this flake.

**And the flake survives the fix**: **2 / 40** post-fix runs of
`smoke-rqstress-liveness` failed (5.0%), against a 2/27 baseline (7.4%) —
statistically indistinguishable, and both failures carry the identical
signature described below. Stated plainly: **DDR-878 fixes a real defect and does not fix the
flake.**

## 3. What the flake actually is, as far as the evidence goes

A failing run was captured with the full serial log. The signature, identical
across the pre-fix catch and both post-fix failures:

- `[boot-stamp] A probe-block-begin` prints.
- `[boot-stamp] B proofs-begin` **never prints.**
- Boot continues normally for 100+ more lines — other threads run, user probes
  spawn and exit, the heartbeat ticks.
- The last thing `fs_test_thread` does is spawn the ext4-rooted probe. It never
  runs again.

So `[smp] rqstress FAIL` is not printed either: `rqstress_proof()` is never
reached. The gate reports "required pattern not found", which reads as a
scheduler proof failing when in fact the proof never ran.

That rules out the two hypotheses DDR-864 listed. It is **not wrong data** (no
bad reads, no stuck-request watchdog line) and **not a block-layer hang** (the
next step after the stall point is an *embedded* ELF load with no disk I/O).
The signature is a single runnable thread lost while every other thread
continues — a per-CPU runqueue / work-stealing loss (the rq-1 `rq_next` /
`rq_on` / `on_cpu` interaction), which is also why the one gate that stresses
the runqueues is the one that flakes.

**Item 47 is therefore documented rather than fixed**, which the item's own
standard allows — but with far more than DDR-863 had: a measured per-gate rate,
four gates cleared, a named subsystem, and a reproducible capture procedure
(`tools`-side scripts run the gate until it fails and keep the serial log).

## 4. Item 50 is still blocked, and by a smaller thing than before

Item 50 needs three consecutive green CI runs on one tip. At ~1/8 on one gate
across a 6-shard matrix, a full-suite green is roughly 88% likely per run and
three in a row roughly 68%. That is a real obstacle but no longer a wall, and it
now has a single named gate behind it rather than "the SMP gates".

## What is proven, and what is not

**Proven:** the wait list is correct by construction and every slot-release path
wakes a waiter; the regression set is green; the old precondition does not occur
in this workload.

**Not proven:** that the wait list is ever exercised at depth ≥2 at all — the
witness says it is not, here. The fix is therefore correct-but-unexercised, and
this DDR says so rather than counting a green suite as evidence for it. A gate
that forces ≥9 concurrent block requests would exercise it, and is the natural
follow-up.

Gates green: `smoke`, `smoke-sysmmap`, `smoke-user`, `smoke-fs`, `smoke-fs-rw`,
`smoke-fs-sfs-rw`, `smoke-fs-ext4`, `smoke-e1000e`, `smoke-ahci`, `smoke-blkmq`,
`smoke-blk-integrity`, `smoke-msixap`, `smoke-sfs-btree-smp4`. Zero warnings
under `-Werror`.
