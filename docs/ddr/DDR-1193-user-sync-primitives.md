# DDR-1193: C1.4 P3, user mutex, semaphore, condition variable and barrier on futex; where priority inheritance goes

Status: **implemented in P3a** (Amendment 1), on `dev/phase1` `cc99745` after #81
(C1.4 P2, `SYS_FUTEX`) merged; P3 is built on that syscall. Not gated until
Y1–Y6 are shown red in CI. Platform: QEMU (TCG, 4 vCPU). Nothing
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
- **Timeouts.**
  - `pmutex` and `psem` retry after a wake, so the caller's relative timeout
    cannot simply be passed to every `futex_wait`. Each retry would restart it,
    and the total wait could exceed what the caller asked for.
  - The `*_timedwait` forms therefore read the vDSO monotonic clock once at
    entry (the same clock the probes already use, `VDSO_USER_VA`) and fix a
    deadline. Each retry passes only the time remaining, and the call returns
    `-ETIMEDOUT` once that is zero.
  - #81 rounds a timeout to 10 ms ticks, so the bound is the deadline plus at
    most one tick.
  - Absolute `CLOCK_REALTIME` deadlines wait for C1.5 P1's `clock_gettime`.

Not in P3: robust mutexes (need `set_robust_list`, C1.4 P4), PI user mutexes
(`FUTEX_LOCK_PI`, after D1), rwlocks and spinlocks (P4 takes them from musl).

## 4. Gate: six new arms in `smoke-thread`

The arms run in the existing probe (`user/threadtest.c`) after FJ, so they
reuse its fork-and-judge structure and bounded waits. Each arm spreads its
threads over the 4 vCPUs with `SYS_SCHED_CPU`, and its result counts only if
every thread confirms its placement (the same rule as arms D and E, DDR-1186).
Every parent-side wait is bounded by the vDSO clock (5 s per arm). A hang
becomes a printed `timeout=1`, not a stuck boot.

**"Asleep" is observed, not assumed.** Where an arm needs a thread to be blocked
inside `futex_wait`, the parent polls `SYS_GETPROCS` until that thread's state is
`THREAD_BLOCKED` (3). The poll is bounded and its result is printed. In these arms
the waiter's only blocking call is `futex_wait`, so BLOCKED means it is asleep on
the futex.
- **Not verified:** that `SYS_GETPROCS` lists a thread by the id
  `SYS_THREAD_CREATE` returned. Arm C already finds `THREADTEST` threads through
  it; P3a checks the id mapping before relying on it.

| Arm | Sentinel (exact) | What it does |
|---|---|---|
| SA mutex exclusion | `PRADYOS_SYNC_A total=80000 exp=80000` plus `[sync] A contended=` > 0 | 4 threads, one per CPU, each run 20 000 iterations of a **non-atomic** read-modify-write on a shared counter under one `pmutex`. `contended` counts `futex_wait` calls in the lock slow path (a probe-only counter), so the arm proves the futex path ran. |
| SW mutex wake | `PRADYOS_SYNC_W asleep=1 woke=1 timeout=0` | The holder takes the `pmutex`. A second thread calls lock, and the parent waits until that thread is BLOCKED. The holder then unlocks once. The waiter must return from lock within 1 s. |
| SB semaphore | `PRADYOS_SYNC_B got=4000 asleep=1 maxin_ok=1 overlap=1 timeout=0` | (1) A consumer waits first and is seen BLOCKED (`asleep=1`) before the producer's first post. Then 4 000 posts and 4 000 waits; `got` must be 4 000. (2) A `psem` initialised to 2 guards a section that 4 threads enter 500 times each. `maxin_ok=1` means the peak count inside never exceeded 2; this is the correctness check. `overlap=1` means two holders were inside at once, which shows the arm contended. To make overlap happen by design rather than by chance, a holder that enters alone stays inside, up to 50 ms by the vDSO clock, until a second holder arrives. |
| SC condvar broadcast | `PRADYOS_SYNC_C asleep=3 woke=3 timeout=0` | 3 waiters wait on one `pcond`. The parent broadcasts only after all 3 are seen BLOCKED. `woke` counts only waiters whose `futex_wait` returned 0 (a real wake), not `-EAGAIN`. |
| SH condvar hand-off | `PRADYOS_SYNC_H rounds=1000 timeout=0` | Two threads on different CPUs alternate a predicate 1 000 times under `pmutex` + `pcond` (signal). A third thread contends for the same `pmutex` in a loop, so a waiter is often asleep on the mutex when `pcond_wait` relocks it. |
| SD barrier | `PRADYOS_SYNC_D phases=100 bad=0 timeout=0` | 4 threads cross a `pbarrier` 100 times. After each crossing, every thread checks that all 4 phase counters are ≥ its own; `bad` counts violations. |

All six sentinels go into the `smoke-thread` `EXTRA_SENTINEL` list.
`GLOBAL_FORBIDDEN` is not touched.

### 4.1 Mutants (one arm each, as CLAUDE.md rule 3 requires; each must be shown red in CI before the gate merges)

| Mutant | Change in `psync.h` | Red on | Expected value |
|---|---|---|---|
| Y1 | `pmutex_lock` becomes a plain load-then-store (no atomic) | SA | `total` < 80 000 (lost updates) |
| Y2 | `pmutex_unlock` never calls `futex_wake` | SW | `woke=0`, `timeout=1` |
| Y3 | `psem_wait` decrements without checking `val > 0` | SB | `maxin_ok=0` |
| Y4 | `pcond_broadcast` wakes 1, not all | SC | `woke=1` |
| Y5 | `pbarrier` last arrival does not bump `gen` | SD | `timeout=1` (all wait forever) |
| Y6 | `pcond_wait` relocks in state 1, not 2 | SH | `timeout=1` (a stranded mutex waiter, §3) |

**Vacuity, per arm, before any arm is written:**
- **SA.** With the mutex removed (Y1), the lost updates have to come from real
  parallel execution. That holds only if QEMU runs the 4 vCPUs in parallel
  (MTTCG) on the runner.
  - **Not verified:** whether `ubuntu-24.04` QEMU with `-smp 4` runs MTTCG for
    x86_64.
  - If Y1 comes back green, SA is vacuous on this platform. I will say so in
    the DDR and not count SA as gated. I will not weaken the expected total.
- **SW.** `asleep=1` proves the waiter really slept before the single unlock, so
  a missing wake (Y2) cannot be covered by the waiter's own retry.
- **SB.** `maxin_ok` is the correctness half. `overlap=1` proves a second holder
  was inside, so a semaphore that over-admits (Y3) has the chance to show it.
- **SC.** All three waiters are seen BLOCKED before the broadcast, and only a
  real wake counts. A waiter that returns `-EAGAIN` because it saw `seq`
  change cannot make `woke=3`, so Y4 must give 1.
- **SH.** Y6 strands a waiter only when a third thread sleeps on the mutex at
  the moment of relock; the contender exists for that. If Y6 stays green
  anyway, I will report SH as not shown red and redesign it, not drop Y6.
- **SD.** Without the generation bump nobody leaves the barrier, so the bounded
  wait reports `timeout=1`.

## 5. Order of work

1. This DDR (docs-only PR).
2. After #81 merges: P3a, one PR with
   - `psync.h`,
   - the six arms,
   - the sentinels,
   - one green run on its exact head,
   - Y1–Y6 dispatched (at most 3 in CI) and each shown red on its arm.
3. Checklist: the C1.4 "mutex, semaphore, condition variable, barrier" row is
   ticked with P3a's merge SHA. "(priority inheritance)" moves to C1.5's
   real-time row (D1).

## 6. Honesty labels

- `psync` primitives: will be **gated (`smoke-thread` SA, SW, SB, SC, SH, SD)**, emulated only,
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

## Amendment 1 (P3a, implementation): what changed from §3–§4, and why

Written before the code, on `dev/phase1` = `cc99745` (#81 merged).

1. **Each arm runs in a forked child.** The `D`/`E`/`F`/`G` pattern is used: the
   child's main thread orchestrates, the threads report through a
   `MAP_SHARED` page, and the parent judges the end with `reap()`. A mutant
   that leaves threads asleep forever (Y2, Y4, Y5, Y6) is then ended by
   `SIGKILL`, which wakes a futex sleeper (FG), and is never a stuck boot.
   `timeout=1` means the child's own 5 s bound expired **or** `reap()` had
   to kill the child.
2. **`placed_ok=1` and `contended_ok=1` join the sentinels.** `grep -F` cannot
   test "> 0", and placement must be part of the verdict (§4). Each thread pins
   with `SYS_SCHED_CPU` op 1 and then confirms with op 0 (GET) that it is on
   its CPU. This proves which CPU each thread ran on, not that the threads ran
   at the same time; the MTTCG question in §4.1 stays open. The exact sentinels
   are:
   - `PRADYOS_SYNC_A total=80000 exp=80000 contended_ok=1 placed_ok=1`
   - `PRADYOS_SYNC_W asleep=1 woke=1 timeout=0 placed_ok=1`
   - `PRADYOS_SYNC_B got=4000 asleep=1 maxin_ok=1 overlap=1 timeout=0 placed_ok=1`
   - `PRADYOS_SYNC_C asleep=3 woke=3 timeout=0 placed_ok=1`
   - `PRADYOS_SYNC_H rounds=1000 timeout=0 placed_ok=1`
   - `PRADYOS_SYNC_D phases=100 bad=0 timeout=0 placed_ok=1`
3. **The thread id mapping (§4, "Not verified") is checked from the code.**
   - `sched_create_user_thread` sets `t->pid = t->tid` (`sched.c:1420`) and returns `t->pid` (`sched.c:1461`).
   - `sched_snapshot` reports `out->pid = t->pid` (`sched.c:1663`).
   - So the id `SYS_THREAD_CREATE` returns is the `pid` that `SYS_GETPROCS` lists.
   - If that were wrong, the BLOCKED poll would never find the thread, and the
     arm would print `asleep=0` on correct code, so it fails visibly, not silently.
4. **SB's linger is counted in vDSO ticks.** The vDSO clock advances 10 ms per
   PIT tick (`idt.c:374`), so "50 ms" style waits have 10 ms granularity.
   - On entry, a holder lingers until a **third** holder is inside, or until the
     clock has advanced one tick (at most 10 ms). It does this only on its first
     20 entries.
   - Correct code never admits a third holder, so that costs at most 80
     entries × 10 ms, shared by two holders at a time (about 0.4 s).
   - Y3 admits all four threads, so a third holder appears inside a linger
     window and `maxin_ok=0`.
   - The same linger gives `overlap=1` (a second holder inside) by design, not
     by chance.
5. **Y6, made precise.** §3 says relocking "with 1" can strand a mutex waiter.
   - That holds for a relock loop that **writes** 1:
     `while (xchg(st, 1) != 0) futex_wait(st, 1)` overwrites the 2 that a
     sleeping waiter set. The next unlock sees 1 and skips its wake, so that
     waiter and the relocker both sleep with `st == 0`.
   - It does not hold for Drepper's `CAS 0→1` fast path. That writes 1 only
     when `st == 0`, and a woken waiter always sets 2 again before it can
     sleep.
   - So `pcond_wait` relocks with `while (xchg(st, 2) != 0) futex_wait(st, 2)`,
     and **Y6 replaces both 2s with 1**.
   - SH's contender keeps `st == 2` common at the moment of relock.
6. **No `*_timedwait` in P3a.** No arm in §4 exercises a timeout, and ungated
   code would break the honesty labels. The timed forms (§3, "Timeouts") are
   added with their own arm and mutant when a consumer needs them (P4, musl).
   Until then they are **not implemented**.
7. **Cost is measured.** `smoke-thread` has a `FORBIDDEN_SENTINEL`, so it has
   no early exit and always runs its full `TIMEOUT_S` (240 s; run 41299374169
   shard 2: `PASS smoke-thread (240s)`). The probe prints
   `[sync] start_ms=` and `[sync] end_ms=` (vDSO clock) around the six arms,
   and `[sync] <arm> ms=` for each, so the CI run shows whether the arms finish
   inside the window. `TIMEOUT_S` changes only if a run shows they do not.
