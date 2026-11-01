# DDR-1193: C1.4 P3, user mutex, semaphore, condition variable and barrier on futex; where priority inheritance goes

Status: **design**. No code until #81 (C1.4 P2, `SYS_FUTEX`) is merged into
`dev/phase1`; P3 is built on that syscall. Platform: QEMU (TCG, 4 vCPU). Nothing
here will be verified on real hardware.

## 1. Where the tree is (read from `dev/phase1` = `65357d3`, plus #81 at `547e1c5`)

- **Futex.** #81 adds `SYS_FUTEX` (NSI 117) with `FUTEX_WAIT` (0), `FUTEX_WAKE`
  (1) and `FUTEX_PRIVATE_FLAG` (128). It has an optional relative timeout, and
  private and shared keys (DDR-1181 Amendment 1). It has no `REQUEUE`, no
  `WAKE_OP`, no PI ops and no robust list.
- **Threads and placement.** `SYS_THREAD_CREATE`/`EXIT` (#76) and
  `SYS_SCHED_CPU` (#91, DDR-1186) let a probe put one thread on each vCPU, so
  contention is real and not only interleaving on one CPU.
- **`kmutex`** (`kernel/proc/kmutex.c`, 35 lines) is a FIFO hand-off sleeping
  lock with no priority inheritance.
- **No priorities.** There is one scheduling class. `tcb.weight` is 1024 for
  every thread (`sched_create_state`, `sched.c:1148`) and only scales the
  vruntime charge (`sched.c:325`). Nothing sets a weight or priority from user
  space. DDR-1188 §1 states the same: "There is one class and no priority."
- **User space has no sync library.** Probes are freestanding: no libc, raw
  syscalls through `user/include/pradyos.h`. `user/threadtest.c` uses ad hoc
  spin flags.

## 2. The contradiction this DDR resolves

DDR-1181 §2 puts "Kernel `kmutex` with priority inheritance" in C1.4 P3, and
the checklist row reads "mutex, semaphore, condition variable, barrier
(priority inheritance)". DDR-1188 §5 (C1.5) says "P3 does not add priority
inheritance". So the two design records disagree, and neither part owns it.

**Priority inheritance cannot be gated in C1.4.** Inversion needs a high-priority
waiter, a low-priority holder and a medium-priority thread that starves the
holder. With one class and equal weights there is no high or low. Any C1.4
"PI" arm would therefore pass on the tree without PI, which makes it vacuous.
CLAUDE.md rule 3 forbids a gate that has never been seen red. Writing PI code
now would also leave it ungated until C1.5.

**Decision D1.** Priority inheritance for `kmutex` moves to **C1.5 P3** (the
part that adds `SCHED_FIFO`/`SCHED_RR`). Its gate goes with it:
- an RT thread blocks on a `kmutex` held by a `SCHED_OTHER` thread;
- an RT spinner shares the holder's CPU (pinned with `SYS_SCHED_CPU`);
- the RT waiter's wait must be bounded;
- a mutant that drops the boost must be shown red.

DDR-1188 §5's limitation line is amended to point here (§7). C1.4 P3 is the user
primitives only. The C1.4 checklist row loses "(priority inheritance)", and the
C1.5 real-time row gains it. **Flagged to the operator on #45**, because it
moves an item between checklist blocks.

## 3. Design: `user/include/psync.h`

The library is header-only and freestanding, like the rest of `user/include`.
It uses GCC `__atomic` builtins with explicit memory orders and one inline
`SYS_FUTEX` wrapper. Every primitive defaults to the private key
(`FUTEX_PRIVATE_FLAG`). A `*_shared` init variant omits the flag for objects in
`MAP_SHARED` memory, which uses the shared key from #81.

| Primitive | Word(s) | Lock / wait | Release / wake |
|---|---|---|---|
| `pmutex` | `u32 st`: 0 free, 1 held, 2 held with waiters (Drepper, *Futexes Are Tricky*, mutex 3) | CAS 0→1, otherwise loop `if (xchg(st,2) != 0) futex_wait(st, 2)` | `if (xchg(st,0) == 2) futex_wake(st, 1)` |
| `psem` | `u32 val`, `u32 nwait` | CAS-decrement while `val > 0`; otherwise `nwait++`, `futex_wait(val, 0)`, `nwait--`, retry | `val++` (release); `if (nwait) futex_wake(val, 1)` |
| `pcond` | `u32 seq` | `s = load(seq)`; unlock the mutex; `futex_wait(seq, s)`; relock the mutex **in state 2** | signal: `seq++`, `futex_wake(seq, 1)`; broadcast: `seq++`, `futex_wake(seq, INT32_MAX)` |
| `pbarrier` | `u32 count`, `u32 gen`, `u32 n` | last arrival: `count = 0`, `gen++`, `futex_wake(gen, INT32_MAX)`; the others: `g = load(gen)` taken *before* arriving, then `while (load(gen) == g) futex_wait(gen, g)` | (inside wait) |

Why each piece is shaped this way:
- **`pmutex` state 2.** Unlock makes a syscall only when someone may be
  sleeping. A waiter always sets 2 before sleeping, so a wake cannot be lost.
  `FUTEX_WAIT`'s compare under the bucket lock (DDR-1181 A1.3) closes the
  window between the waiter's `xchg` and its sleep.
- **`pcond` relocks in state 2.** After a wake, other waiters may still be
  asleep on the mutex. Relocking with 1 could let the next unlock skip its wake
  and strand them. This is the classic lost-wakeup bug.
- **`pcond` seq read before unlock.** A signal that comes after the unlock but
  before the `futex_wait` changes `seq`, so the wait returns `-EAGAIN` instead
  of sleeping through it.
- **No requeue.** Broadcast wakes all waiters, and they contend on the mutex
  (a thundering herd). This is a stated cost. `FUTEX_REQUEUE` is not in #81,
  and adding it is a kernel ABI change outside P3.
- **Timeouts.** The `*_timedwait` forms pass #81's relative timeout and return
  `-ETIMEDOUT`. Absolute deadlines wait for C1.5 P1's `clock_gettime`.

Not in P3: robust mutexes (need `set_robust_list`, C1.4 P4), PI user mutexes
(`FUTEX_LOCK_PI`, after D1), rwlocks and spinlocks (P4 takes them from musl).

## 4. Gate: four new arms in `smoke-thread`

The arms run in the existing probe (`user/threadtest.c`) after FJ, so they
reuse its fork-and-judge structure and bounded waits. Each arm spreads its
threads over the 4 vCPUs with `SYS_SCHED_CPU`, and its result counts only if
every thread confirms its placement (the same rule as arms D and E, DDR-1186).
Every parent-side wait is bounded by the vDSO clock (5 s per arm). A hang
becomes a printed `timeout=1`, not a stuck boot.

| Arm | Sentinel (exact) | What it does |
|---|---|---|
| SA mutex | `PRADYOS_SYNC_A total=80000 exp=80000 timeout=0` plus `[sync] A contended=` > 0 | 4 threads, one per CPU, each run 20 000 iterations of a **non-atomic** read-modify-write on a shared counter under one `pmutex`. `contended` counts `futex_wait` calls in the lock slow path (a probe-only counter), so the arm proves the futex path ran. |
| SB semaphore | `PRADYOS_SYNC_B got=4000 maxin=2 timeout=0` | (1) A producer posts 4 000 times and a consumer waits 4 000 times; `got` must be 4 000. (2) A `psem` initialised to 2 guards a section that 4 threads enter 500 times each; `maxin`, the peak count inside, must be exactly 2. Not more, which would be a broken count. Not fewer, which would mean the arm never contended. |
| SC condvar | `PRADYOS_SYNC_C rounds=1000 bcast=3 timeout=0` | (1) Ping-pong: two threads on different CPUs alternate a predicate 1 000 times under `pmutex` + `pcond` (signal). (2) Broadcast: 3 waiters block on one `pcond`; one broadcast must release all 3 (`bcast=3`). |
| SD barrier | `PRADYOS_SYNC_D phases=100 bad=0 timeout=0` | 4 threads cross a `pbarrier` 100 times. After each crossing, every thread checks that all 4 phase counters are ≥ its own; `bad` counts violations. |

All four sentinels go into the `smoke-thread` `EXTRA_SENTINEL` list.
`GLOBAL_FORBIDDEN` is not touched.

### 4.1 Mutants (one per arm; each must be shown red in CI before the gate merges)

| Mutant | Change in `psync.h` | Red on | Expected value |
|---|---|---|---|
| Y1 | `pmutex_lock` becomes a plain load-then-store (no atomic) | SA | `total` < 80 000 (lost updates) |
| Y2 | `pmutex_unlock` never calls `futex_wake` | SA | `timeout=1` (a sleeper is never woken) |
| Y3 | `psem_wait` decrements without checking `val > 0` | SB | `maxin` > 2 |
| Y4 | `pcond_broadcast` wakes 1, not all | SC | `bcast=1` |
| Y5 | `pbarrier` last arrival does not bump `gen` | SD | `timeout=1` (all wait forever) |
| Y6 | `pcond_wait` relocks in state 1, not 2 | SC | `timeout=1` (a stranded mutex waiter, §3) |

**Vacuity, per arm, before any arm is written:**
- SA. With the mutex removed (Y1), the lost updates have to come from real
  parallel execution. That holds only if QEMU runs the 4 vCPUs in parallel
  (MTTCG) on the runner.
  - **Not verified:** whether `ubuntu-24.04` QEMU with `-smp 4` runs MTTCG for
    x86_64.
  - If Y1 comes back green, SA's lost-update half is vacuous on this platform.
    I will then say so and gate SA on `contended` and Y2 only, not weaken the
    expected total.
- SB. `maxin` = 2 is reachable only if two threads are inside at once. The
  lower bound in the sentinel catches a vacuous run.
- SC. The broadcast half needs all three waiters asleep before the broadcast.
  The probe waits until each has bumped an "about to wait" counter, then waits
  10 ms more. Even then, a waiter that has not yet reached `futex_wait` sees
  `seq` changed and returns, which also counts as released. So `bcast=3` holds
  under either interleaving, and Y4 is red only when a waiter was really asleep.
  - **Not verified** until Y4 is red in CI.
- Y6 is the subtle one. It strands a waiter only when a third thread is asleep
  on the mutex at the moment of relock. The ping-pong half alone may not hit
  that. If Y6 stays green, I will add a third contender to SC rather than
  drop Y6.

## 5. Order of work

1. This DDR (docs-only PR).
2. After #81 merges: P3a, one PR with
   - `psync.h`,
   - the four arms,
   - the sentinels,
   - one green run on its exact head,
   - Y1–Y6 dispatched (at most 3 in CI) and each shown red on its arm.
3. Checklist: the C1.4 "mutex, semaphore, condition variable, barrier" row is
   ticked with P3a's merge SHA. "(priority inheritance)" moves to C1.5's
   real-time row (D1).

## 6. Honesty labels

- `psync` primitives: will be **gated (`smoke-thread` SA–SD)**, emulated only,
  **not verified on real hardware**. Memory-order claims rest on the x86-64 TSO
  model under TCG. They are not tested on a weakly ordered CPU (the riscv64 and
  aarch64 bootstraps do not run user threads).
- Priority inheritance: **not implemented, not gated** until C1.5 P3 (D1).
- Fairness: none. `pmutex` is not FIFO (a running thread can barge), and
  `pcond` broadcast is a thundering herd. Stated, not gated.

## 7. Amendments to other records (same PR)

- DDR-1181 §2, the P3 row: "Kernel `kmutex` with priority inheritance; …" becomes
  "User mutex, semaphore, condition variable and barrier on top of futex
  (DDR-1193). `kmutex` priority inheritance moved to C1.5 P3 (DDR-1193 D1)."
- DDR-1188 §5, the "Priority inversion, unbounded" bullet: "P3 does not add
  priority inheritance" becomes "P3 adds `kmutex` priority inheritance, with
  the gate in DDR-1193 D1".
- `docs/V1_MASTER_CHECKLIST.md`: C1.4 row "mutex, semaphore, condition
  variable, barrier (priority inheritance)" loses "(priority inheritance)";
  C1.5 "real-time classes (FIFO, RR, deadline)" gains "with `kmutex` priority
  inheritance (DDR-1193 D1)".
