# DDR-1139: OPEN-2's double dispatch. `schedule_locked` re-queues a `prev` that `sched_unblock` has already queued, which creates a second runqueue token

**Status:** DESIGN committed before the code (§NON-NEGOTIABLE 5, `76a06ec`); **FIX SHIPPED** — §7 records the regression. **Confirmation is the fixed-kernel hunt, pending.**
**Date:** 2026-09-24
**Asked by:** the operator on PR #17 (comment 5808782807, author_association OWNER): *"I want OPEN-2's root cause NAMED and a FIX SHIPPED … The moment you have enough to name an actual mechanism … fix it immediately."*
**Scope:** `kernel/proc/sched.c` (`schedule_locked`) plus one heartbeat field.

---

## 1. The artefacts

Hunt **35963517515** ran 20 lanes × 55 runs on pinned `ca8107ec7f5d8de7`, which is `a390eab` at `OPEN2_HUNT=32`. I rebuilt that binary **bit-for-bit** in a detached worktree, so every address below resolves against the exact binary that ran. Four lanes were non-success: 3, 5, 14 and 15. Their DONE lines read:

| lane | signal_runs | churn_runs |
|---|---|---|
| 3 | 1 | 55 |
| 5 | 1 | 54 |
| 14 | 1 | 55 |
| 15 | 1 | 55 |

The other 16 lanes succeeded, and their DONE lines were not fetched. Only failed-job logs are read, as in DDR-1134.

### 1.1 Lane 5: the pre-registered double-resume row, with its precondition present, for the first time

```
[schedcheck] … rq_on=1 disp=44 saves=42 …
```

DDR-1131 §3 pre-registered this row before any such data existed: *"`disp == saves + 2` → **DOUBLE RESUME CONFIRMED**, DDR-1118's mechanism named, with `rq_on=1` corroborating via its stated precondition."*

- Every earlier `saves+2` fire (DDR-1133, DDR-1134) read `rq_on=0`. That absence is exactly why DDR-1133 §10 kept four readings open.
- This fire carries **both** halves.
- Its heartbeat window reads `calls=3416 bails=2861`. In other words, 84% of the bounded on-CPU waits in that window gave up.

### 1.2 Lane 15: a saved `rsp` below its own stack

```
[schedcheck] next->rsp invalid tid=11 pid=0 rsp=0x07C2FF08 base=0x07C30000 … (rsp outside its own stack) rq_on=0 disp=36960 saves=36959 halting.
```

- **tid 11 is `fs_test_thread`.** The kernel threads are created in this order: bench, recv, send, prod, cons, three bus threads, blk, reaper, fs. A temporary local stack-painting build confirmed it by printing `tid=11 name=fs`.
- The saved `rsp` is **0xF8 bytes below the base of its own 16 KiB stack**.
- There are exactly two writers of `->rsp` in the tree: the create-time seed (`sched.c:1193`) and `context_switch(&prev->rsp, …)`. So this value was saved while `fs` was the outgoing thread.

It is not an ordinary stack overflow. I measured the high-water mark with a painted-stack build: in a full healthy `-smp 4` boot, `fs` uses at most **10,832 B** and leaves **5,552 B free**.

What the measurement is consistent with is **two CPUs executing `fs` at once on one stack**. That is the consequence §2 derives.

The heartbeats from lane 15's freeze onward read **`calls=250 bails=250`** in every window. Every on-CPU wait gives up: a CPU is waiting for a thread that will never go off-CPU.

### 1.3 Lanes 3 and 14

**Lane 3** is `saves+1` with `rq_on=0`. Its RFLAGS slot is `0xFFFFFFFF8014DCD8`, which is `&g_rq[3].lock`, a pointer and not a flags word. So a frame on that stack holds values written by *other* code, which is what two CPUs sharing one stack produces. The CPU that halted is vblk unit 1's MSI-X destination. That is why unit 1's completions time out for the rest of the boot: the vblk symptom is downstream of the halt, which is DDR-1115's causal chain again.

**Lane 14** is the vblk `compl_lock` spin (`spin_lock_contended+0x92 ← submit+0x2e`). Two CPUs are frozen, and unit 2's `compl_lock` reads `waiters=2`. A thread running on two CPUs at once, inside `submit`, is one way to reach a state no single-threaded holder could.

**None of 1.2–1.3 is claimed as proof of the mechanism.** They are consistent with it. The proof is in the code (§2) and the measurement (§3).

## 2. The mechanism: two live tokens for one thread

A thread is dispatchable when it holds a **token**: an entry in some runqueue (`rq_on=1`), or having been popped by exactly one picker. `rq_push` deduplicates with an atomic exchange on `rq_on`, but that only deduplicates tokens **still sitting in a queue**. Once a picker has popped the token, `rq_on` is 0 and a second push succeeds.

Only three places ever make a thread READY, and each one pushes:

1. `sched_create_state` (`THREAD_READY` at insert)
2. `schedule_locked`'s own RUNNING→READY transition
3. `sched_unblock`'s CAS from BLOCKED to READY

`schedule_locked` (`sched.c:1543-1546`) then does this:

```c
if (prev->state == THREAD_RUNNING)
    prev->state = THREAD_READY;
if (prev->state == THREAD_READY && !prev->is_idle)
    rq_push(cpu, prev);
```

The second test also matches a `prev` that entered **already READY**, meaning one that `sched_unblock` woke *before* its own `schedule()` ran. That is the ordinary shape of every block-and-wake. `sched_block_timeout` stores `BLOCKED`, drops the caller's lock, and calls `schedule()`, and a completion interrupt on another CPU can land in that gap.

`sched_unblock` has **already pushed that thread's token**. If another CPU B has popped it by now (`rq_on=0`), B holds it and is spinning in `switch_wait_offcpu_sched` for `prev` to go off-CPU. The re-queue above then pushes a **second** token onto this CPU's queue.

Now suppose a third CPU C pops the second token before this CPU finishes switching away. B and C are then both spinning on the same `prev->on_cpu`. `finish_task_switch` release-stores `-1`, **both acquire loads see it, and both claim**. The claim is three plain stores:

```c
next->on_cpu = cpu;
next->state = THREAD_RUNNING;
next->dispatches++;
```

Nothing in them is exclusive, so both CPUs `context_switch` into the **same saved `rsp`**.

The result is `dispatches` two ahead of `switches_away` (lane 5, `saves+2`), and `rq_on=1` when a claim races a still-queued token (lane 5). It is also two CPUs pushing frames onto one kernel stack. That produces every "consumed frame", "wrong slot contents" and "rsp outside its own stack" signature this investigation has recorded since DDR-1099.

**The comment beside the wait states the invariant this breaks:** *"(next != prev here — the keep-running case returned above, so we never wait on ourselves.)"* That is false for the same reason. This CPU can pop **its own** unblock token (via `rq_pop`, or `rq_steal` from the waker's queue). When it does, `next == prev` and `switch_wait_offcpu_sched` waits on its own `on_cpu` until the 4096-spin bail. Lane 15's `calls == bails` in every window is that signature.

### 2.1 What this does to DDR-1131 §2

DDR-1131 §2 closed the bail path **by reading** and was correct as far as it went: the bail returns its own token and does not double-resume. The duplicate is created **earlier**, by the `prev` re-queue, and the bail path is only where it becomes visible. DDR-1133 §5 recorded that a candidate closed on a reading must be re-opened for measurement once an artefact touches it. §3 is that measurement.

## 3. Measured precondition on the pre-fix tree (a temporary counter build, reverted)

A local, never-committed build added three counters in `schedule_locked`:

- `tkdup`: `prev` READY on entry, `next != prev`, and `rq_on == 0`. This is the push that creates a second token.
- `tkself`: `next == prev`.
- `tkdbl`: the claim's old `on_cpu` was already ≥ 0, meaning a double claim.

| gate | rc | tkdup | tkself | tkdbl |
|---|---|---|---|---|
| smoke-rqstress (first build, `tkdup` also counting self-picks) | 0 | 1 | 1 | 0 |
| smoke-blk-integrity | 0 | 0 | 0 | 0 |
| **smoke-smpuser** | 0 | **1** | 0 | 0 |

So on the pre-fix tree **the second token is created in an ordinary healthy boot**. A double *claim* needs a further race (a third CPU popping the duplicate inside the switch window), which is why it is rare. The per-boot rate of about 0.2–0.4% (DDR-1132, DDR-1136, DDR-1137) is the rate of that second race, not of the first.

**NOT CLAIMED:** one `tkdup` per healthy boot is a count, not a rate. `tkdbl` was not observed locally.

## 4. The fix

1. **Re-queue `prev` only on this function's own RUNNING→READY transition.** A `prev` that entered already READY was queued by `sched_unblock`, which owns that token, so it is not pushed again. After this change a thread has exactly one token per transition to READY.
2. **If `next == prev`, keep running.** This CPU popped its own token, so it is the only holder. It sets RUNNING and returns, with no wait on itself, no 4096-spin bail and no switch. A `next == prev` that is somehow not READY falls back to the idle pick.
3. **Make the claim exclusive as defence in depth.** `next->on_cpu` goes from -1 to this CPU by CAS. If the CAS fails, some *other* token source exists; this CPU drops its copy (no push, because the other claimer owns it) and runs idle. Each such event increments `g_dbl_claim`, printed in `[hb]` as `dblclaim=`. After (1) and (2) this should read 0 forever. A non-zero value means a token source this DDR did not find, and it is then an artefact rather than a silent double run.

The false comment beside the wait is corrected in the same change.

**Cost:** one `lock cmpxchg` replaces a plain store on the claim, on the path that already does two atomic operations (`rq_on` and the `on_cpu` acquire). (2) removes up to 4096 `sti;pause;cli` iterations per self-pick.

## 5. How it is proven

1. **Mechanism:** the code path in §2, whose precondition is measured in §3.
2. **Fixed tree, same counters:** the push that creates the duplicate no longer exists, so a local run of the three counter gates on the fixed tree must show that the self-pick path no longer waits (`bails` in `[hb]` falls) and `dblclaim=0`.
3. **Regression:** smoke-shell 5/5, smoke-smp, smoke-smppreempt, smoke-rqstress, smoke-blk-integrity, smoke-blkmq and smoke-smpuser, all `rc=0`, with the hash pinned.
4. **The discriminating measurement is the hunt**, because that is where the double claim was ever observed.
   - Recent dispatches on the pre-fix binary: 3–4 signal runs per 1,100 boots.
   - A 20×55 hunt of the fixed kernel with 0 signals gives P(0 | p = 0.3%) ≈ e^−3.3 ≈ 0.037. Two such dispatches give ≈ 0.001.
   - **This is set before the data: zero `[schedcheck]` and zero vblk-spin `[apfreeze]` across 2,200 fixed-kernel boots counts as confirmation. Any fire is read against its own binary first (§INV.18) and, together with `dblclaim=`, says whether a token source remains.**

## 6. Not claimed

- **Not claimed that every OPEN-2 producer is this one.** DDR-1133 §6's silent panic in `cap.c`'s `resolve` (`loser_vec=13`) and DDR-1134 §5's lane-12 panic have no report that names them. If they persist after this fix, they are separate defects and will be named as such (the operator's step 4).
- **Not claimed that lane 14's vblk freeze is proven to be this mechanism.** It is consistent with it, and the hunt will say.
- `switch_wait_offcpu_sched`, `rq_push`, `rq_take` and `finish_task_switch` are each correct for what they do. The defect is the interaction in §2.
- The false comment at the wait site is the text-level form of the defect.
- `GLOBAL_FORBIDDEN` stays at 77 and the gate count stays at 179. `dblclaim=` is an instrument, not a sentinel.

## 7. Shipped, and the local regression

**Kernel `22ce5984de925d38`, 1,319,306 B (size unchanged).** Warning-clean at `-Werror`. The hash was pinned and re-checked after every gate (DDR-1060 §9). `KEEP_SERIAL=1`, logs are under `build/gatelogs/f1139-*`, and every row below is `rc=0`:

| gate | dblclaim (last `[hb]`) | calls / bails summed over all `[hb]` windows |
|---|---|---|
| smoke-shell ×5 | — (early exit, no heartbeat) | — |
| smoke-smp | 0 | 1,486,925 / 161 |
| smoke-smppreempt | 0 | 1,946,234 / 203 |
| smoke-rqstress | 0 | 1,870,782 / 219 |
| smoke-blk-integrity | 0 | 1,928,840 / 200 |
| smoke-blkmq | 0 | 1,866,686 / 213 |
| smoke-smpuser | 0 | 1,947,550 / 208 |
| smoke-resched | 0 | (last window 57,922 / 1) |
| smoke-nethammer, smoke-rqfree | (not read: capture path / early exit) | — |

`hygiene_check.sh` passes ALL NINE. `GLOBAL_FORBIDDEN` stays at 77 and the gate count stays at 179.

### 7.1 A correction to §5.2, made against my own prediction

§5.2 predicted that `bails` in `[hb]` would *fall* on the fixed tree. **It does not.** Here is the same gate on the pre-fix counter build (§3) and on the fixed tree:

| smoke-blk-integrity | calls | bails | bail rate |
|---|---|---|---|
| pre-fix (counter build) | 1,816,309 | 164 | 0.009% |
| fixed | 1,928,840 | 200 | 0.010% |

Pre-fix `smoke-smpuser` read 230 / 1,936,721, which is 0.012%.

- On a healthy boot, the self-pick path is taken about once per boot (§3, `tkself=1`). The ~200 ordinary bails come from legitimate on-CPU contention that this change does not touch.
- So `bails` **cannot discriminate** the two trees on a healthy boot, and it is withdrawn as a proof arm.
- It stays a useful reading in a **failing** capture. Lane 15's `calls == bails` in every window is a regime no healthy boot shows at any level: a 100% bail rate against 0.01%.

What the local regression licenses:

- The fix does not break the fifteen gate runs (fourteen distinct gates, smoke-shell ×5).
- The defence-in-depth claim was never needed on these boots (`dblclaim=0`).

What it does **not** license: a claim that OPEN-2 is fixed. The double claim was never observed locally (§3, `tkdbl=0`). **§5.4's hunt criterion, set before the data, is the confirmation, and it is still pending.**
