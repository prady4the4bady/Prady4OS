# DDR-1157 — checkpoint/resume: find + act under g_sched_lock (Fix 2)

Operator-approved, PR #27 comment 5970979125. Fix 1 is DDR-1156; this is Fix 2.

## §1 Defect (confirmed from source)

`sys_checkpoint_agent` and `sys_resume_agent` (`kernel/syscall/sys_checkpoint.c`,
NSI 84/85, both CAP_SOVEREIGN) do:

```c
struct tcb *t = sched_find_pid(pid);   /* returns a RAW pointer, no lock held */
if (!t) return -ESRCH;
t->checkpointed = 1;                    /* checkpoint: deref AFTER the lookup   */
...
t->checkpointed = 0;                    /* resume: deref AFTER the lookup       */
if (t->state == THREAD_BLOCKED) sched_unblock(t);
```

Two things are unlocked, and both are a **use-after-free** on an SMP machine:

1. **The lookup itself.** `sched_find_pid` (`sched.c:2504`) walks the all-threads
   ring by `->next` with **no lock**, while the reaper relinks that ring under
   `g_sched_lock` (`sched_ring_unlink`, caller "MUST hold g_sched_lock"). A walk
   racing an unlink can follow a `->next` that is being rewritten.
2. **The dereference after the lookup returns.** The reaper frees a TCB like this
   (`sched_destroy`, and the `reaper_thread` path):

   ```c
   irq_save();            /* == spin_lock_irqsave(&g_sched_lock) */
   sched_ring_unlink(t);  /* removes t from the ->next ring, UNDER the lock */
   irq_restore(fl);       /* releases the lock */
   sched_free_tcb(t);     /* kfree(t), OUTSIDE the lock */
   ```

   So between `sched_find_pid` returning `t` and the syscall dereferencing it,
   another CPU can unlink `t` and `kfree` it. The write (`t->checkpointed = …`)
   or read (`t->state`, `sched_unblock(t)`) then lands in freed, PMM_POISON-
   stamped memory. This is the same freed-while-referenced class as DDR-996,
   reached through a different door (an operator syscall rather than a runqueue
   walk).

Confirmed by reading `sys_checkpoint.c`, `sched_find_pid`, `sched_ring_unlink`,
`sched_destroy` and `sched_free_tcb` directly. No reproduced crash is claimed —
see §6.

## §2 Why holding g_sched_lock across find + act closes it

The free is **preceded by `sched_ring_unlink` under `g_sched_lock`**. Therefore,
while a critical section holds `g_sched_lock`:

- No thread can be unlinked from the ring (the unlink needs the lock we hold).
- A TCB that is **still in the ring is not yet unlinked, hence not yet freed**
  (free in `sched_free_tcb` runs only after the unlink, which is serialised by
  the lock).
- A TCB that has **already been unlinked is not reachable by a ring walk** at all
  — `sched_find_pid`'s walk follows `->next`, and an unlinked node is off the
  ring — so the walk never returns a pointer that is on its way to being freed.

So a find + use performed entirely inside one `g_sched_lock` critical section
only ever touches a live, unfreeable TCB. This is exactly what the operator
prescribed ("checkpoint pointer lifetime under g_sched_lock").

## §3 The fix

Two helpers in `sched.c`, each taking `g_sched_lock` for the whole find + act
(`irq_save`/`irq_restore` are the file-local g_sched_lock wrappers):

```c
int sched_checkpoint_pid(uint32_t pid) {       /* set checkpointed = 1 */
    uint64_t fl = irq_save();
    struct tcb *t = find_pid_locked(pid);       /* ring walk, excl ZOMBIE/DONE */
    int rc = -ESRCH;
    if (t) { t->checkpointed = 1; rc = 0; }
    irq_restore(fl);
    return rc;
}

int sched_resume_pid(uint32_t pid) {           /* clear + conditional unblock */
    uint64_t fl = irq_save();
    struct tcb *t = find_pid_locked(pid);
    int rc = -ESRCH;
    if (t) {
        t->checkpointed = 0;                    /* clear BEFORE unblock (DDR-837
                                                 * order: else it re-blocks) */
        if (t->state == THREAD_BLOCKED)
            sched_unblock(t);
        rc = 0;
    }
    irq_restore(fl);
    return rc;
}
```

`sys_checkpoint.c` calls these instead of `sched_find_pid` + a raw deref. Every
other line of the syscall — the `is_sovereign`/CAP_DENIED check, the `-EINVAL`
rejections of pid 0/1/self, and the `aether_audit` records — stays in the syscall
layer, unchanged.

**`find_pid_locked` is a lock-free-walk helper CALLED WITH THE LOCK HELD**; it is
the body of `sched_find_pid` minus the lock-less assumption. `sched_find_pid`
itself is left in place (other callers exist) — this DDR does not change its
contract; it stops the two checkpoint syscalls from relying on it across a
dereference.

### §3.1 Calling `sched_unblock` under `g_sched_lock` is safe

`sched_unblock` takes no `g_sched_lock`; it does a BLOCKED→READY CAS, a leaf
`rq_push`, and `smp_resched_one` (a directed IPI). The lock order is **outer
(`g_sched_lock`) → leaf (`rq`)**, which is the allowed direction:
`sched.c:96` states the rq lock "is a LEAF: never held while taking g_sched_lock
or another rq lock", so no CPU ever holds an rq lock while waiting on
`g_sched_lock` — there is no cycle. The rq-2 schedule hot path takes no
`g_sched_lock` either, so the IPI target cannot deadlock against us.

The established batch convention (the DDR-955 sweep releases `g_sched_lock`
before `sched_unblock`) is a **hold-time** optimisation on the hottest lock in
the kernel, not a safety rule. Here the alternative — release the lock, then
`sched_unblock(t)` — is **rejected**, because after releasing the lock `t` can be
freed, which reintroduces the exact UAF this fix removes. Checkpoint/resume is a
CAP_SOVEREIGN operator action (cold path), so the extra hold time is irrelevant.

## §4 Gate

`smoke-checkpoint` (shard 3, strict; `user/ckpttest.c`) already exercises both
syscalls end to end and is the functional gate: arm 1 checkpoints a live target
and asserts it reaches `THREAD_BLOCKED`; arm 2 resumes it and asserts it leaves
`THREAD_BLOCKED`; arm 3 asserts `-ESRCH` for an unknown pid; arm 4 asserts
`-EPERM` for a non-sovereign caller; the controller asserts `-EINVAL` for init.
The refactor preserves every one of these, so the gate stays green and proves the
helpers are functionally correct and are the live path (PRADYOS_CKPT_OK requires
both helpers to work). No new gate is added (DDR-1039/1070 reasoning: a second
gate booting an OS to drive the same syscalls buys nothing).

## §5 Mutation check

The non-vacuity proof is **M2**. M1 was tried first, is recorded here because it
was wrong in an instructive way, and is **not** the proof.

- **M1 (resume order inverted) — EQUIVALENT UNDER THE LOCK, gate PASSES.** Clear
  `checkpointed` AFTER `sched_unblock` instead of before. I predicted arm 2 would
  fail ("target wakes, reaches its next syscall, still sees `checkpointed == 1`,
  re-blocks") and **it did not — the gate passed, exit 0.** The prediction is the
  DDR-837 reasoning for the *unlocked* code, and it is false for the *locked*
  helper: `t->checkpointed = 0` and `sched_unblock(t)` both execute with
  `g_sched_lock` held across **both**, so the target cannot run between them. The
  intra-critical-section order of two writes the target never observes mid-section
  has no effect the gate (or anything) can see. So M1 is an **equivalent mutant**,
  not a caught one — and that is itself a small confirmation of §2: the lock that
  closes the UAF also serialises these two writes against the target.
  (The BEFORE order still ships: it is the correct order the instant the lock is
  released at some future point between the two, and it costs nothing to keep.)
- **M2 (ESRCH dropped) — CAUGHT, this is the proof.** Return `0` instead of
  `-ESRCH` when the pid is absent (`int rc = 0;` in `sched_checkpoint_pid`).
  `smoke-checkpoint` arm 3 fails with exit 2 and the exact line
  `CKPT FAIL: checkpoint of an unknown pid was not -ESRCH rc=0` — so the gate is
  non-vacuous for the `-ESRCH` contract the refactor must preserve (a helper that
  silently reported success for a pid it never found would pass a gate that did
  not check this). Mutant kernel hash `dcfb892a9cbdc252`; reverting returns the
  shipped `f5124d9b145df8cb` bit-for-bit.

**Measured, not argued:** baseline fixed kernel `f5124d9b145df8cb` passes
`smoke-checkpoint` (exit 0); M2 fails it (exit 2) at arm 3; M1 passes it (exit 0)
and is therefore not a discriminating mutant.

### §5.1 Regression (scheduler-touching change)

The fix adds a lock-free ring-walk helper and two `g_sched_lock` critical
sections to `sched.c` and reroutes two cold-path syscalls, so the scheduler and
block paths were re-run on the shipped kernel `f5124d9b145df8cb`, one QEMU at a
time (NON-NEGOTIABLE 12): `smoke-shell` **5/5** (global-forbidden scan clean, 77
patterns), `smoke-blk-integrity`, `smoke-blkmq`, `smoke-rqstress-liveness` — all
rc=0. Static checks: `ci-shard-check` OK (187 gates, no new gate added),
`ci-probe-rodata-check` OK, `ci-start-align-check` OK. `kernel.bin` is
1,474,954 B — size unchanged from the pre-fix tree, so the CLAUDE.md
size/headroom pair and `ci-docstate-check` are unaffected.

## §6 Not claimed — stated plainly

**The gate does NOT reproduce the UAF, and no gate can, deterministically.** The
race is **strictly SMP**: on a single CPU there is no yield between
`sched_find_pid` returning and the dereference, so no other thread — and so no
reaper free — can run in that window. Reproducing it needs a second CPU freeing
the target in a few-instruction window, which is the same irreducibly-timing
problem OPEN-2 has; `smoke-checkpoint` runs single-CPU and would pass a
lock-removed mutant. So:

- The UAF closure rests on the **structural argument in §2** (free is preceded by
  an unlink under `g_sched_lock`; the helper now holds that lock across find +
  act), confirmed by reading the reaper and free paths — not on a reproduced-
  then-fixed artefact. The operator approved Fix 2 on exactly that read.
- **No rate is claimed.** The window needs the operator to checkpoint/resume a
  pid at the instant that thread is being reaped on another CPU — rare, which is
  why no hunt or gate has surfaced it.
- U1/U2/U3 and OPEN-1/OPEN-2 do **not** move; no mechanism is named for any of
  them. This removes one UAF on the checkpoint/resume path and nothing more.
- `sched_find_pid`'s own contract is unchanged; other callers are out of scope.
