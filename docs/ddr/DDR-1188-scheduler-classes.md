# DDR-1188: Scheduler classes, high-resolution time and pressure stall information (checklist C1.5)

Status: design ahead (#45 comment 6436706603, item 2). No code until C1.4 has
merged (`Needs: C1.4`); the arms below pin threads with `SYS_SCHED_CPU`
(DDR-1186, #91).

## 1. Where the tree is (read from `dev/phase1` = `7981cd4`)

- **Tick.** 100 Hz: `pit_init(100)` on the BSP and `lapic_timer_100hz()` on
  each AP (`kernel/main.c`). Every timeout in the ABI is in milliseconds or
  ticks, so the real resolution is 10 ms.
- **Picker.** Per-CPU run queues (`struct rq { lock; head, tail; }` in
  `kernel/proc/sched.c`). `rq_pop` does not take the head: it calls
  `fair_candidate()` and unlinks the queued thread with the smallest
  `dbg_vruntime` (DDR-895), so the pick is already vruntime-fair. A running
  thread is preempted when its quantum expires: `QUANTUM` is 2 ticks (20 ms).
  There is one class and no priority; `tcb.weight` feeds the vruntime charge.
- **Clocks.** `SYS_CLOCK` (57) returns seconds since midnight from the RTC, and
  `SYS_TIME` (72) copies a broken-down `struct rtc_time`. The kernel already
  calibrates the TSC against the PIT at boot (`calibrate_tsc_hz` in
  `kernel/main.c`), and the vDSO page carries a monotonic-since-boot clock
  (`kernel/vdso/`). What is missing is the ABI: no `clock_gettime` with
  POSIX clock ids and nanosecond results, and no `nanosleep`. P1 adds those on
  top of the existing calibration and vDSO clock; it does not build a clock
  from nothing.
- **Pressure.** Nothing measures time spent waiting for a CPU, for memory or
  for I/O.

## 2. Scope split (one PR per part, in this order)

| Part | Content | Needs |
|---|---|---|
| P1 | The time ABI on the existing TSC calibration and vDSO clock: `clock_gettime(CLOCK_MONOTONIC, &ts)` and `clock_gettime(CLOCK_REALTIME, &ts)` (a `struct timespec`, nanosecond resolution), `nanosleep`. `CLOCK_REALTIME` is defined in K6 | C1.4 |
| P2 | One-shot LAPIC timers (TSC-deadline mode where CPUID has it, else one-shot count), so a timer can fire between ticks | P1 |
| P3 | Scheduling classes: `SCHED_FIFO` and `SCHED_RR` (priority 1-99; a higher number runs first; at equal priority, FIFO and RR threads share one queue in arrival order, and only RR rotates on its timeslice) ahead of the existing vruntime-fair class, which becomes `SCHED_OTHER`; RT throttling | P2 |
| P4 | `SCHED_DEADLINE`: EDF with a constant-bandwidth server and admission control | P3 |
| P5 | Pressure stall information for CPU and I/O (blocked block-device reads), as `some`: the fraction of the last 1 s (100 ticks, sampled each tick, a plain sliding window with no exponential decay) in which at least one thread was stalled; memory once C2.1 can stall on reclaim | P3 |
| P6 | Tickless idle (stop the periodic tick on an idle CPU, wake on the next timer); CPU frequency only where the hardware exposes it | P2 |

`SCHED_OTHER` keeps today's picker unchanged: `fair_candidate()` and the
2-tick quantum. P3 only puts the RT classes in front of it. Changing the fair
picker itself is out of scope, because OPEN-2 timing lives on this path.

## 3. Decisions (delegated to me by order 4; my recommendations, taken)

- **K1. Who may use an RT or deadline class.** Only a thread whose capability
  table holds a new `RES_SCHED` capability with an RT right. init holds it and
  can grant it. Agents never get it (`-EPERM`), because an agent at RT priority
  could starve the supervisor. Without the capability, `sched_setscheduler`
  to FIFO, RR or DEADLINE returns `-EPERM`.
- **K2. RT throttling.** On each CPU, RT and deadline threads together get at
  most 950 ms of every 1 s (Linux's default). In the rest, `SCHED_OTHER` runs,
  so a FIFO spinner cannot lock out the shell or the reaper.
- **K3. RR timeslice.** 100 ms, a whole number of ticks, until P2 makes finer
  slices possible.
- **K4. Admission for `SCHED_DEADLINE`.** The sum of `runtime/period` over a
  CPU's deadline threads may not exceed 0.90, which leaves room for K2's
  floor. Over-subscription returns `-EBUSY`.
- **K5. CPU frequency.** QEMU TCG exposes no P-states. P6 reads CPUID for
  HWP/EIST and reports "not available" otherwise. Any frequency feature is
  labelled **not verified on real hardware** until it runs on a real machine.
- **K6. `CLOCK_REALTIME`.** Seconds and nanoseconds since the Unix epoch,
  UTC. At boot the kernel reads the RTC once and records `realtime_base` =
  that wall time minus the current `CLOCK_MONOTONIC` value; `CLOCK_REALTIME`
  is then `realtime_base + CLOCK_MONOTONIC`, so its sub-second part comes from
  the TSC, not from the PIT-tick vDSO counter. P1 has no `clock_settime` and
  no NTP: the base is never adjusted, so `CLOCK_REALTIME` drifts with the TSC
  and steps only across a reboot. The RTC is read in UTC (the QEMU default);
  a local-time RTC gives a wrong offset and is recorded as a limitation.
- **K7. vDSO layout and update protocol (P1).** Today `vdso_data_t`
  (`kernel/vdso/vdso_page.h`) holds only `wall_time_ns`, advanced from the PIT
  IRQ, and a reserved `seq`. P1 keeps `wall_time_ns` at offset 0 for existing
  readers and adds, in the padding: `tsc_base` (TSC at the last update),
  `ns_base` (monotonic nanoseconds at `tsc_base`), `mult` and `shift` (TSC
  ticks to nanoseconds, from `calibrate_tsc_hz`) and `realtime_base` (K6).
  The writer makes `seq` odd, stores the fields, then makes `seq` even, with
  release ordering. A reader loads `seq`, retries while it is odd, reads the
  fields, and retries if `seq` changed; `CLOCK_MONOTONIC` is then
  `ns_base + ((rdtsc - tsc_base) * mult >> shift)`. None of this exists yet;
  until P1, the tree has no `CLOCK_MONOTONIC` or `CLOCK_REALTIME` at all.

## 4. Gate `smoke-schedclass` (one probe, `user/schedtest.c`; arms by part)

Timing under TCG is not hardware timing, so every arm asserts an ordering or a
bound with a wide margin, never a precise duration.

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| A | P1 | `CLOCK_MONOTONIC` never goes backwards over 10 000 reads on each of two pinned CPUs, and across CPUs: a thread on CPU 0 writes its reading to shared memory and a thread on CPU 1 reads it and then its own clock, and the same in the other direction, 10 000 times each way; the receiving CPU's clock is never below the value it just read from the other CPU. A constant offset in either direction fails one of the two directions | C1 | read the TSC without the per-CPU offset (one CPU lags or leads by a constant; a lag fails the 0-to-1 direction, a lead fails the 1-to-0 direction) |
| B | P1 | `nanosleep(25 ms)` returns after at least 25 ms by the monotonic clock (25 ms is not a whole number of 10 ms ticks, so rounding down to ticks returns at 20 ms) | C2 | round the sleep down to whole ticks |
| C | P2 | a 2 ms one-shot timer fires before the next periodic tick (count of early wakes > 0 over 50 tries) | C3 | arm the periodic tick only |
| D | P3 | a FIFO thread woken on a CPU busy with a `SCHED_OTHER` spinner runs within one tick of its wake, before the spinner's 2-tick quantum ends | C4 | queue RT threads in the fair class (picked by `fair_candidate`, so they wait for the quantum) |
| E | P3 | two RR threads of equal priority on one CPU both make progress | C5 | RR never rotates (behaves as FIFO) |
| F | P3 | two FIFO threads of equal priority: the second does not run until the first blocks, across at least 5 quantum expiries | C6 | FIFO rotates on quantum expiry, as `SCHED_OTHER` does |
| G | P3 | with a FIFO spinner on a pinned CPU, a `SCHED_OTHER` thread on the same CPU accrues at least 4 run ticks (40 ms of CPU time at the 100 Hz tick) in each of 5 consecutive 1 s windows of `CLOCK_MONOTONIC` time; CPU time is its own `run_ticks`, read through `SYS_GETPROCS` (DDR-754), not elapsed time (K2 gives it 50 ms; 40 ms leaves margin under TCG) | C7 | no RT throttling (the `SCHED_OTHER` thread gets 0 ms) |
| H | P3 | `sched_setscheduler(FIFO)` without `RES_SCHED` returns `-EPERM`; init granting `RES_SCHED` to an agent is refused (`-EPERM`), and that agent's `sched_setscheduler(FIFO)` then returns `-EPERM` | C8, C8b | skip the capability check; allow the grant to an agent |
| I | P4 | admission refuses the deadline thread that would take a CPU over 0.90 (`-EBUSY`) | C9 | no admission check |
| J | P4 | two admitted deadline threads on one pinned CPU, A (period 10 ms) and B (period 40 ms). On each of 100 contended releases, B is made runnable first and A second, and both are runnable before the CPU picks: A runs first in at least 95 of the 100, and neither misses a deadline | C10 | EDF ordering ignored (deadline threads picked in arrival order, so B, which arrived first, runs first) |
| K | P5 | CPU `some` pressure is above 0 with two spinners on one CPU, and 0 when that CPU is idle | C11 | never account runnable-wait time |
| L | P6 | an idle CPU takes fewer than 5 ticks in one second | C12 | tick never stopped |
| M | P2 | an unprivileged thread re-arming a 50 µs one-shot as fast as it can gets at most 1 100 early wakes in one second (budget 1 000 plus margin) | C13 | no per-thread budget |
| N | P3 | two FIFO threads, each holding `RES_SCHED` granted by init (K1), on one pinned CPU, priority 10 and priority 20; the priority-10 thread is made runnable first and spins; the priority-20 thread is then woken and runs within one tick | C14 | priority order reversed (lower number wins) |
| O | P5 | I/O `some` pressure is above 0 while a thread is blocked on a 4 MiB uncached virtio-blk read, and back to 0 within 2 s after it completes (the 1 s window has then emptied) | C15 | blocked block-device reads not accounted |

Before each arm is written, I check it for vacuity: it must fail on the tree
without its part. Arm D, for instance, fails on today's picker because there is
no wake-up preemption: the woken thread waits for the spinner's 2-tick quantum
to expire. That is read from the code and is confirmed on the baseline before
the arm is written.

## 5. Risks

- **OPEN-2 and OPEN-12/13 live on the scheduling path.** P3 changes the picker.
  Every P3 PR runs the whole existing gate set, and any new `[apfreeze]` or
  hang signature is a stop, not a retry.
- **Priority inversion, unbounded.** A FIFO thread blocked on a kmutex held by
  a `SCHED_OTHER` thread waits until that holder releases the lock. K2's 50 ms
  floor only guarantees `SCHED_OTHER` CPU time; it does not bound when a
  particular holder gets to run, nor a holder that blocks on something else
  while holding the lock. Without priority inheritance the RT wait has no
  bound. P3 adds `kmutex` priority inheritance, with the gate in DDR-1193 D1
  (an RT waiter on a kmutex held by a `SCHED_OTHER` thread, an RT spinner on
  the holder's CPU, a bounded wait, and a mutant that drops the boost).
  Amended by DDR-1193: this bullet previously said P3 does not add it.
- **Timer interrupts are a security boundary.** Two controls, both needed:
  - spacing: a one-shot deadline is clamped to at least 50 µs after the
    previous timer interrupt on that CPU. Spacing alone still allows 20 000
    interrupts per second.
  - budget: a thread without `RES_SCHED` may have at most one pending
    one-shot timer and at most 1 000 expiries per second. Past the budget, a
    re-arm is rounded up to the next periodic tick. `RES_SCHED` holders get
    the full rate, and agents never get it. A new arm M checks this: an
    unprivileged thread re-arming as fast as it can takes at most about 1 000
    early wakes per second (mutant C13: no budget).

## 6. Honesty labels

- Nothing here is built. Each part is **not verified** until its arms are
  green and its mutants are shown red in CI.
- Everything will be emulated (QEMU TCG) only, and **not verified on real
  hardware**; TCG time is not wall time, so the arms assert orderings and wide
  bounds only.
- This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 1 (2026-11-15): arm A's mutant had nothing to remove

**Finding (before any P1 code, by reading `dev/phase1` at `2f54a7e`).**
- Arm A's mutant C1 is "read the TSC without the per-CPU offset". The tree has no per-CPU TSC offset: no `IA32_TSC_ADJUST` write, no offset field in `struct percpu`, and no skew measurement at AP bring-up.
- Under QEMU TCG every vCPU's `rdtsc` comes from one virtual clock, so the TSCs are already in step.
- C1 therefore names a change with nothing to change. Arm A, guarded only by C1, could never be shown red: a gate that proves nothing (CLAUDE.md rule 3).

**Decisions.**

- **A1-1. No per-CPU offset in P1.** P1 reads `rdtsc` directly.
  - At AP bring-up, each AP's skew against the BSP is measured once, by a round trip (CodeRabbit on #129: a one-way transfer folds the latency into the skew).
    - The AP drives it, because the AP is the CPU being measured and the BSP is busy waiting for it to come online. The AP reads its TSC (`ta0`) and posts a request.
    - The BSP's wait loop sees the request and answers with its own TSC (`tb`).
    - The AP reads its TSC again (`ta1`) when the answer arrives.
    - This is repeated 8 times, and the sample with the smallest round trip `ta1 - ta0` is kept.
    - Skew (AP minus BSP) is `(ta0 + ta1) / 2 - tb`, signed, with a bound of ±`(ta1 - ta0) / 2`.
    - Ordering (CodeRabbit on #129). `RDTSC` is not serializing (Intel SDM: an `LFENCE` before it waits for earlier loads; an `LFENCE` after it keeps later instructions from starting first). The reads are ordered as follows:
      - `ta0` is `rdtsc; lfence`, before the request store;
      - `tb` is `lfence; rdtsc; lfence`, after the load that sees the request and before the answer store;
      - `ta1` is `lfence; rdtsc`, after the load that sees the answer.
      - Each asm carries a `memory` clobber, so the compiler cannot move the handshake's loads and stores across it.
  - The result is printed as `[time] tsc skew cpu=<n> cyc=<skew> rt=<round trip>`, in TSC cycles, because the TSC is not yet calibrated when the APs come up.
  - It is a measurement, not a correction, and it does not decide anything by itself.
- **A1-1b. The skew P1 accepts is the skew arm A tolerates.** P1 sets no threshold of its own (an earlier draft said 1 µs; CodeRabbit on #129 showed that a skew below 1 µs can still make arm A's cross-CPU read go backwards).
  - Arm A's cross-CPU directions are the acceptance test.
  - If arm A fails on a machine and that run's measured skew explains the backward step, the fix is a per-CPU offset, under its own amendment. The offset would be applied through `IA32_TSC_ADJUST`, or through a per-CPU offset read with `RDTSCP`'s `TSC_AUX`.
  - Until that fix lands, P1 is recorded as **not verified** on that machine.
  - Under QEMU TCG the vCPU TSCs come from one clock, so this case is not expected in CI. Every CI run prints its measured skew, so the claim is checked on each run, not assumed.
- **A1-2. Arm A's mutant is replaced.** The new C1: the vDSO writer advances `ns_base` by a fixed 10 ms per PIT tick, instead of recomputing it from the TSC at each update. `tsc_base` is still refreshed at each update.
  - This mixes two time sources. A reader between ticks interpolates with the TSC. When a tick arrives late (more than 10 ms of TSC time since the last update, which under TCG is routine), the reader has already seen more than `ns_base + 10 ms`. After the update it reads about `ns_base + 10 ms`, so the clock steps backwards.
  - The correct writer sets `ns_base` from the TSC: `ns_base += (tsc_now - tsc_base) * mult >> shift`, then `tsc_base = tsc_now`. The clock is then monotonic by construction.
  - C1 fails arm A's same-CPU check whenever any tick in the 10 000-read window is late. That is probabilistic, so P1 records how many backward steps each C1 run saw. If a CI run shows C1 green, C1 is recorded as not gated and arm A is redesigned. It is not merged on a guess.
- **A1-3. The seqlock read is stated not gated.** A mutant that drops the reader's `seq` retry (call it C1b) would need a reader to land inside the writer's few-instruction window. P1 does not claim to gate it. It is listed so the gap is visible.
- **A1-4. Syscall numbers.** 120 is `SYS_CLOCK_GETTIME (clockid, struct timespec *)` and 121 is `SYS_NANOSLEEP (const struct timespec *, struct timespec *)`. Both are free on `dev/phase1` at `2f54a7e`. The vDSO path is the fast path; the syscall is the fallback and the reference.
- **A1-5. Priority inheritance and tickless idle.** Two notes carried from the review of the withdrawn DDR-1204 (#127):
  - P3's kmutex inheritance must move a boosted `SCHED_OTHER` owner onto the RT queue at the inherited priority, and back when the boost ends. Otherwise the owner can wait behind a lower RT thread and never release the lock.
  - P6's tickless idle must program a wake for the K2 throttle's budget replenishment, so a throttled RT thread on an idle CPU runs again.

Everything in this amendment is design; nothing is built. QEMU only, not verified on real hardware.

## Amendment 2 (CodeRabbit on #130, before merge)

Two findings on the P1 code (#130, head `b3a319d`), checked against the code and accepted.

- **A2-1. A blocked `nanosleep` must wake on a signal.**
  - **The defect.** `sig_raise` wakes a blocked thread only through `futex_kick`, which acts on `t->fx_lock`. P1's sleep set `block_lk` but never `fx_lock`, so a handled signal or `SIGKILL` that arrives during the sleep is not seen until the block ends. A `nanosleep(1 h)` made its process unkillable for an hour. The `-EINTR` path ran only for a signal already pending between blocks.
  - **The fix.**
    - The sleeper takes its static lock (`g_nsleep_lk`) once, sets `t->fx_lock` to it, and loops under it: deadline check, then signal check, then `sched_block_timeout`.
    - `futex_kick` takes the same lock before `sched_unblock`. A signal raised after the check therefore waits until the wait is published, then wakes it. No wake is lost.
    - `fx_lock` is cleared before the lock is dropped.
    - **Reusing `fx_lock`.** It is the one "wake me on a signal" hook `sig_raise` already calls. A thread is never in a futex wait and a sleep at the same time.
  - **Return value.** A sleep whose deadline has passed returns 0 even if a signal is also pending, as on Linux: the signal is then delivered on return to ring 3. `-EINTR` writes the remaining time to `rem`.
  - **Arm B2.** Two forked sleepers each call `nanosleep(5 s)` and set a ready flag in a shared page first. The parent waits for the flag, then waits until `SYS_GETPROCS` reports the sleeper `THREAD_BLOCKED`, so the signal arrives during the block and not before it (CodeRabbit on #130). A sleeper that never blocks fails the arm.
    - It sends the first sleeper `SIGUSR1`, which has a handler. That sleeper must return `-EINTR` with 0 < `rem` < 5 s.
    - It sends the second sleeper `SIGKILL`.
    - The parent times each `wait4` from the moment the signal was sent. Both must take less than 2 s.
    - Sentinel: `PRADYOS_SCHED_B2 eintr_ok=1 killed_ok=1`.
    - **Vacuity:** on `b3a319d` both sleepers run the full 5 s, so the arm is red.
    - **Mutant C2b:** never set `fx_lock`. Both waits run out, so arm B2 is red.
- **A2-2. Ordered TSC sample in the reader.**
  - `ktime_mono_ns()` and the probe's vDSO reader sample the TSC with `lfence; rdtsc`, the pattern A1-1 uses.
  - Without the fence, `rdtsc` may execute before the `seq` and base loads that precede it. The sample would then not belong to the base it is combined with.
  - P1's clamp of a negative delta to 0 already kept the result at or above `ns_base`. The fence makes the reading's order explicit instead of relying on that clamp.
  - This is not gated separately (as with C1b, A1-3): a mutant would need a reordering that TCG does not produce.
- **A2-3. Default-terminate signals end the sleep too** (CodeRabbit on #130, second review).
  - An unmasked `SIGTERM` or `SIGPIPE` with no handler is woken by A2-1's kick. But `signal_interruptible` (which requires a handler) and `signal_kill_pending` (`SIGKILL` only) both miss it, so the sleeper blocked again, and default termination waited out the whole sleep.
  - **The fix:** a new predicate, `signal_fatal_pending(t)`. It covers an unmasked pending `SIGTERM` or `SIGPIPE` with no handler, the set `signal_deliver` ends a thread on (DDR-805). The sleep checks it alongside the other two and returns `-EINTR`, and `signal_deliver` then ends the thread on the way out.
  - **Arm B3:** a third forked 5 s sleeper, with no handler, gets `SIGTERM` 200 ms after it is ready. It must be reaped within 2 s. Sentinel: `PRADYOS_SCHED_B3 term_ok=1`.
    - **Mutant C2c:** drop `signal_fatal_pending` from the check. The sleeper then runs about 5 s, so arm B3 is red.
    - **Vacuity:** on `22c3f09` the arm is red.
  - B2 and B3 also treat as a failure a `kill` that fails, a `wait4` that does not reap the child, and an exit status other than the expected one (CodeRabbit). The expected status is 0 for the handled-signal sleeper, which exits by itself, and -1 for the two the kernel ends. Otherwise a child that had already exited would read as a fast kill.
  - **Not changed here:** `FUTEX_WAIT` (`kernel/proc/futex.c`) has the same gap for default-terminate signals. It predates this PR, so the futex path is not widened without its own gate. DDR-1206 fixes it, with arm FK in `smoke-thread`.

QEMU only, not verified on real hardware.

## Amendment 3 (2026-11-16): P2 design, one-shot timers on the LAPIC count

Design only. No P2 code exists yet. It fills in what §2 leaves open for P2: who arms a one-shot, how one LAPIC timer serves both the tick and a one-shot, and how arms C and M can be red. Read from `dev/phase1` at `b2181de`.

**What exists.**
- Each CPU has one LAPIC timer. It runs in periodic mode at 100 Hz (`lapic_timer_100hz`, `lapic_timer_ap_arm`), divide 16, `g_timer_count` counts per 10 ms.
- Its interrupt (vector 48) runs `timer_tick` on the BSP and `sched_tick` on an AP (`idt.c`).
- A timed wait ends in `sched_timed_sweep`, on a tick, when `g_ticks` reaches its deadline.
- `nanosleep` (P1) blocks for `rem / 10 ms + 1` ticks. So a 2 ms sleep ends on the first tick at or after 2 ms: between 2 ms and 12 ms after it began.

**T1. The only user in P2 is `nanosleep`.** No new syscall. A sleeper whose deadline is not on a tick gets a one-shot at its deadline. The tick timeout stays as the backstop, unchanged. If no one-shot can be armed (T4, T5), the sleep behaves as it does today.

**T2. One timer, two jobs: leave periodic mode only while a one-shot is pending.** This is the safe option, and it is a deviation from §2 (below).
- A CPU with no pending one-shot keeps today's periodic mode, untouched. The tick path that OPEN-2 timing lives on does not change for it.
- **Arming** (on the sleeper's CPU, interrupts off). Read the current count: `rem`, the counts left to the next tick. Convert the time to the deadline into counts: `h`.
  - If `h + guard >= rem`, do nothing to the LAPIC. The deadline is checked on the tick.
  - Else switch the LVT to one-shot, write `h` as the initial count, and keep `rem - h` as `tail`.
- **A one-shot interrupt (early phase).** Wake every sleeper on this CPU whose deadline has passed, by the clock. If another deadline lies before the tick, arm it and subtract it from `tail`. Otherwise write `tail` as a one-shot (tail phase). No tick work runs: `g_ticks`, `pc->ticks`, `run_ticks` and the timed sweep do not see this interrupt.
- **The tail interrupt is the tick.** It runs today's tick work, then puts the LVT back to periodic with `g_timer_count`.
- **Every mode switch is followed by a write of the initial count.** A mode change may disarm the timer (next section), so `rem` is read before the LVT is written, and no path leaves the LVT changed without a new count.
- A deadline that the timer reached early by rounding is not woken. It is armed again for the remainder (at least T5's 50 µs).

**Deviation from §2: no TSC-deadline mode in P2.** §2 says "TSC-deadline mode where CPUID has it, else one-shot count". P2 builds the count path only.
- CI's QEMU CPU model (`qemu64`; `boot_test.sh` passes `-cpu` only when `QEMU_CPU` is set) does not report TSC-deadline. Code for it could not run in any gate.
- A write to the LVT that changes the timer mode disarms the timer (Intel SDM vol. 3A, "APIC Timer"; section number not checked against the current edition). TSC-deadline also takes its deadline from an MSR, not the count registers. So it needs its own arming and tail logic, not a variant of T2's.
- It stays a named gap: **not built, not verified.**

**T3. Where the pending one-shots live.** Each CPU has a small table (8 slots) of `{thread, deadline}` and a lock, `hr_lk`, taken with interrupts off.
- A full table means no one-shot for that sleep (tick backstop).
- **Lock order: `hr_lk`, then the sleep lock.** `nanosleep` arms before it takes its sleep lock and cancels after it drops it. The interrupt holds `hr_lk` and wakes through `futex_kick`, which takes the sleep lock. `nanosleep` never takes `hr_lk` while it holds the sleep lock.
- **A slot never outlives its sleep.** `nanosleep` cancels its slot on every way out, including `-EINTR`. A slot that is present under `hr_lk` therefore names a thread that is still inside `sys_nanosleep`, so its tcb is live.
- **No lost wake.** The interrupt wakes a slot only when the clock is at or past its deadline. The sleeper checks the same clock under the sleep lock before it blocks, and `futex_kick` takes that lock. So the sleeper has either seen the deadline pass, or is blocked when the kick arrives.
- A sleeper may resume on another CPU. It cancels by the CPU index it stored when it armed.

**T4. Budget** (§5, "timer interrupts are a security boundary"). A thread gets at most 1 000 one-shots per budget window. Past that, its sleeps get no one-shot and end on a tick. `RES_SCHED` does not exist before P3, so in P2 the budget applies to every thread.
- **The window is fixed, not sliding.** It is 1 s of `CLOCK_MONOTONIC` and opens at the thread's first one-shot after the previous window ended. So a 1 s span that straddles two windows can hold up to 2 000 one-shots. A sliding bound would need a per-thread history; this is the cheap form, and its limit is stated.
- A long sleep counts as one one-shot, taken when the sleep begins.
- Two new `struct tcb` fields (window start, count), with explicit initialisers in `sched_create_state` (CLAUDE.md rule 11).

**T5. Spacing.** A one-shot is never armed closer than 50 µs after the previous timer interrupt on that CPU, and never with a count of 0.

**What the tick loses.** Each switch out of and back into periodic mode restarts the count, so that CPU's next tick is late by the time the two writes take. With T4 that is bounded by 1 000 switches per thread per second. `CLOCK_MONOTONIC` is on the TSC (P1) and is not affected. `g_ticks` on the BSP can run slow by that amount under load. This is **stated, not measured.**

**No wake-up preemption.** A woken sleeper is made ready. It runs when its CPU next schedules: at once if the CPU is idle, else at the end of the running thread's quantum. P2 does not change that (arm D is P3's). So an early wake is only visible on a CPU that is not busy.

### Arm C, restated so that it can be red

§4's arm C says "a 2 ms one-shot timer fires before the next periodic tick (count of early wakes > 0 over 50 tries)". By elapsed time alone it is vacuous: with ticks only, a 2 ms sleep already ends in under 10 ms about 8 times in 10 if the sleeps are spaced at random.

- **The arm.** A thread pinned to CPU 2, with no other load there, runs 50 back-to-back tries of `nanosleep(2 ms)`, timed by `CLOCK_MONOTONIC`.
- **A try is early when** it lasted at least 2 ms and under 4 ms.
- **Why back-to-back tries cannot be early on ticks alone.** A tick-timed sleep ends on a tick. The next try then starts just after that tick, so it ends on the next one, about 10 ms later. Each try is phase-locked to the tick, whichever CPU's tick ends it.
- **Pass:** at least 25 of 50 tries are early, and the pin succeeded. Sentinel: `PRADYOS_SCHED_C early_ok=1 pin_ok=1`, with the counts on a `[sched] C` line.
- **`nowall` is data only.** It counts tries across which the BSP's `wall_time_ns` did not move. A first draft used it as the test. It proves less: a tick on the sleeper's own CPU can end a sleep without the BSP's clock moving.
- **Vacuity, measured locally (QEMU TCG, one boot each, not CI):**
  - tree with P2: `early=49 of 50`;
  - mutant C3: `early=3 of 50`.
- **Mutant C3** ("arm the periodic tick only"): `hr_oneshot` never touches the LVT, so a slot is seen only on a tick.
  - Red on arm C.
  - It is also red on arm M (`sleeps=67`), through M's lower bound, which exists to catch a kernel with no one-shots. Recorded, as C2b was on B2 and B3.

### Arm M

- **The arm.** One thread first sleeps 1.05 s. That sleep takes one one-shot when it begins, so more than a window passes with no new one. The thread then calls `nanosleep(50 µs)` in a loop for 1 s of `CLOCK_MONOTONIC` and counts the calls that returned. The loop's first sleep opens a new window, and the whole second lies inside it.
- **Pass:** the count is at most 1 100. With T4, at most 1 000 calls get a one-shot; each of the rest ends on a tick. The 1 000 one-shot sleeps take at least 50 ms together, so at most 96 ticks are left in the second. Sentinel: `PRADYOS_SCHED_M capped_ok=1`, with the count on a `[sched] M` line.
- **Vacuity, measured locally (QEMU TCG, one boot each, not CI):**
  - tree with P2: `sleeps=1034`;
  - mutant C13: `sleeps=3354`, so TCG does reach a rate the budget must cut;
  - mutant C3: `sleeps=67` (no one-shots), caught by the lower bound.
- **A lower bound too:** the count must be at least 500. Otherwise a kernel that arms no one-shot at all (about 100 per second) would pass arm M.
- **Mutant C13** ("no per-thread budget"): the budget check is removed. Locally, red on arm M only; arm C passes (`early=45 of 50`).

### Review fixes on #136 (Codex and CodeRabbit, before merge)

Five findings, each confirmed by reading `e1652e2`:

1. **An empty table never went back to periodic mode.** With no slot, `hr_earliest` returns all-ones, and `dl + HR_SPACE_NS` wraps. So the tail handler re-armed a one-shot at every tick: every tick then took `hr_lk` and wrote the LAPIC, forever, after the first one-shot. Fixed: the comparison requires a slot.
2. **The budget window was anchored at boot** for a thread whose first one-shot came in the first second (`hr_win` starts at 0). Fixed: a thread's first arm opens its window.
3. **`now` was sampled before `hr_lk`.** The count `rem` was read after it, so `tick_at` was early by the lock wait. Fixed: re-sampled under the lock, beside the count read.
4. **`last_irq` was stale while periodic.** The fast path does not record it, so the first one-shot after a quiet spell could be armed under 50 µs after a tick. Fixed in `hrt_arm`: in periodic mode the previous interrupt was one period before the next tick, so `last_irq` is set to `tick_at - 10 ms`. This keeps the per-tick fast path at two loads, instead of adding a clock read to every tick on every CPU.
5. **The boundary clamp undid the T5 floor.** When the 50 µs floor reached `tick_at`, the target was reset to `tick_at`. Fixed: the target stays at the floor, as a tail. That tick then comes up to 50 µs late, inside the tick delay already stated above.

**Not gated:** none of the five is caught by arms B, C or M, which see only sleep lengths and counts. Fix 1 in particular passed every arm while the CPU stayed in one-shot mode. They are fixed by reading and stated here; no mutant would be red on them.

**Not changed:** the request to update `docs/build_status.md` (`AGENTS.md`). Its last entry is DDR-1178 P1. The project records state in `V1_MASTER_CHECKLIST.md` and `FEATURE_STATUS.md`, as `CLAUDE.md` names them, and DDR-1173 declined a third status list. Whether to revive the file is the operator's call.

### Not gated in P2

- T5's 50 µs spacing. No arm separates it from T4. A mutant that drops it would be recorded as a survivor.
- The table-full path (more than 8 pending sleeps on one CPU).
- A sleeper that resumes on another CPU and cancels there.
- The size of the tick delay described above.
- TSC-deadline mode (not built).

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 4 (2026-11-17): P3 design, FIFO and RR in front of the fair picker

Design only; no P3 code exists when this is written. It fills in what §2–§5 leave open for P3. Read from the P2 head `a10315a` (#136).

**What exists.**
- Each CPU has one run queue, `g_rq[cpu]`: a singly linked FIFO of `tcb`s, one link per thread (`rq_on`), with a lock. Every path that makes a thread runnable pushes through `rq_push`. A pinned thread is queued only on its CPU (DDR-1186).
- `rq_pop` picks `fair_candidate()` (smallest `dbg_vruntime` among READY entries), and falls back to `rq_take` (the head) when it returns nothing. `schedule_locked` picks *before* it re-queues `prev`, so `prev` is never a candidate for its own replacement.
- `sched_tick` charges `run_ticks`, and calls `schedule()` when the 2-tick quantum (`QUANTUM`) runs out. There is no wake-up preemption.
- `kmutex` (DDR-1181 §3.3) hands ownership to its first waiter, FIFO. No priority inheritance.
- Capabilities are per-process tables (`cap.h`). Kernel-only grants follow the two-layer `exec_grant` form (DDR-1034): a kernel-set flag plus a handle checked by `cap_authorize`.
- There is no user-space init. Probes, agents and the supervisor are launched by the kernel's boot path (`main.c`).

**S1. One queue, two pickers.** RT threads stay on the same per-CPU list. No second list, so `rq_on`, the steal path, `rq_unlink`, the DDR-1139 token rules and the OPEN-2 instrumentation see one structure, as today.
- Each `tcb` gets `sched_policy` (OTHER, FIFO, RR), `rt_prio` (1–99, 0 for OTHER) and `pi_prio` (S6). Its *effective priority* is `max(rt_prio, pi_prio)`; a thread is *RT* when that is above 0.
- `rq_pop` first runs `rt_candidate()`: the READY entry with the highest effective priority, and the earliest in list order among equals. So FIFO and RR of one priority share one queue in arrival order, as §2 states.
- If that finds nothing, or the CPU is throttled (S4), `fair_candidate()` runs as today but skips RT entries. If it then finds nothing, `rq_take` takes the head as today. So a throttled CPU with no `SCHED_OTHER` work keeps running its RT threads instead of idling.
- With no RT thread in the system the pick is unchanged. A global count `g_rt_threads` (threads whose effective priority is above 0) gates the new code, so a boot with no RT thread does one extra load per pick and per tick.
- **Stated limit:** priorities are per CPU. A higher-priority RT thread queued on another CPU does not preempt this one. Stealing (`rq_steal`) is unchanged and does not look at priority. The P3 arms pin every RT thread.

**S2. Preemption happens on the tick.** When `g_rt_threads > 0`, `sched_tick` scans the local queue under its lock and calls `schedule()` when:
- the current thread is `SCHED_OTHER` and an RT entry is READY (arm D), or
- the current thread is RT and a READY entry has a higher effective priority (arm N), or
- the current thread is RR, its 10-tick slice (K3) has run out, and a READY entry of equal effective priority waits (arm E), or
- the current thread is RT, the CPU is throttled, and a `SCHED_OTHER` entry is READY (arm G).
An RT current thread is never preempted by quantum expiry alone (arm F). A `SCHED_OTHER` current thread keeps today's quantum rule unchanged.
- No IPI. A wake on another CPU is seen at this CPU's next tick, so the bound is one tick, which is what arms D and N assert.
- **Head re-queue.** An RT thread preempted by a higher priority or by the throttle goes back to the *head* of the queue, not the tail (`rt_head`, consumed in `rq_push`). POSIX puts it at the head of its priority list, and without it a throttle slot would rotate two equal FIFO threads (arm F). An RR thread whose slice ran out goes to the tail.

**S3. `sched_setscheduler` and `RES_SCHED` (K1).**
- **NSI 122 `SYS_SCHED_SETSCHED (tid, policy, prio)`.** `tid` must be 0 (the calling thread) in P3, so a policy never changes while its thread is queued. `policy` is 0 OTHER, 1 FIFO, 2 RR; `prio` is 1–99 for FIFO and RR, 0 for OTHER. Returns 0, `-EINVAL` or `-EPERM`. Moving to OTHER needs no capability.
- **`RES_SCHED` (resource type 7) with two rights:** `CAP_SCHED_RT` (bit 26), to use FIFO and RR, and `CAP_SCHED_GRANT` (bit 27), to grant `RES_SCHED` to another process. Both bits are free on `dev/phase1`.
- **One layer: the handle `sched_cap`, checked by `cap_authorize`.** DDR-1034 pairs a kernel-set flag with the handle and tests them apart. That needs a case with the flag but no handle and one with the handle but no flag, and the P3 arms have neither: a forked child would lack both, so a mutant dropping either check would survive (the masking DDR-1033 records). With fork revoking the handle, one check is the whole boundary, and one mutant (C8) is red on it.
- **Who is "init".** K1 says init holds `RES_SCHED` and grants it. This kernel has no user-space init: the boot path in `main.c` launches every process. So the kernel's launcher plays init. It calls a kernel-only `sched_grant(t, rights)`, which refuses (returns `-EPERM`) any `tcb` with `is_agent` set. In P3 it is called for the schedclass probe only. The supervisor (`is_sovereign`) does not get it in P3, because nothing in it needs RT yet. That is the safe option, and it is said on #45.
- **NSI 123 `SYS_SCHED_GRANT (pid, rights)`.** The caller needs `CAP_SCHED_GRANT`. It grants `rights & (CAP_SCHED_RT)` to the process `pid` through `sched_grant`, so a granted process cannot grant on, and an agent target gets `-EPERM`. Returns 0, `-EPERM`, `-ESRCH` or `-EINVAL`.
- **Fork drops it.** The child of a fork gets its copy of the handle revoked (`cap_fork` copies every slot), `sched_cap = CAP_NULL`, and `SCHED_OTHER`, as Linux's `SCHED_RESET_ON_FORK`. Otherwise every process forked by a holder would inherit RT. A new thread of the same process (`CLONE_VM`) shares the table, as all its capabilities, but starts as `SCHED_OTHER`; it copies `sched_cap`, as it copies `fs_cap`.
- New `tcb` fields: `sched_policy`, `rt_prio`, `pi_prio`, `rr_left`, `rt_head`, `sched_cap`, `kmx_held`. Each gets an explicit initialiser in `sched_create_state` (CLAUDE.md rule 11).

**S4. Throttling (K2) as 19 of every 20 ticks.** On each CPU, in each window of 20 ticks of that CPU's own count, RT threads run at most 19.
- In each aligned 20-tick window RT runs at most 19 ticks, so each aligned 100-tick span (one second at 100 Hz) holds at most 95 RT ticks: K2's 950 ms. A sliding 100-tick span can hold 96, because it can include parts of six windows whose non-RT ticks fall outside it (CodeRabbit on #138; an earlier draft claimed 95 for every span, which was wrong). The sliding bound is 960 ms per second.
- **Why not one 50 ms gap per 1 s window, as Linux does.** Arm G counts the `SCHED_OTHER` thread's run ticks in 1 s windows of `CLOCK_MONOTONIC`. Under TCG the tick rate drifts from 100 Hz. With one 5-tick gap per 100 ticks, a tick rate a little under 100 Hz makes the gap's period longer than 1 s, so some 1 s windows hold no gap and arm G fails at random. With one gap every 20 ticks, a 1 s window at 80–120 Hz holds 4–6 gaps.
- Cost: a CPU running an RT spinner and a `SCHED_OTHER` thread switches twice every 200 ms instead of twice a second.
- A throttled CPU with no READY `SCHED_OTHER` entry keeps running RT (S1, S2). The throttle guarantees `SCHED_OTHER` time; it does not idle the CPU.

**S5. RR timeslice (K3):** 10 ticks (`rr_left`), reset when the thread is picked after a rotation, and when it sets RR.

**S6. `kmutex` priority inheritance (DDR-1193 D1; A1-5).**
- **Boost.** A waiter about to block in `kmutex_lock` raises the owner's `pi_prio` to its own effective priority, if that is higher. This is done under `m->lk`. A boosted `SCHED_OTHER` owner is then RT for S1 and S2: it is picked and preempts at the boosted priority, which is A1-5's "move it onto the RT queue". A queued owner is seen by the next tick on its CPU, without a re-queue.
- **Hand-off.** `kmutex_unlock` hands the lock to the first waiter, FIFO as today. The new owner's `pi_prio` becomes the highest effective priority among the waiters still queued.
- **Drop.** The releasing thread's `pi_prio` goes to 0 when it holds no other `kmutex` (`kmx_held` reaches 0). While it still holds one it keeps the boost. This is conservative: the boost lasts until its last `kmutex` is released, never shorter than needed.
- **Stated limits:** one level only (an owner blocked on a second `kmutex` does not pass the boost on), and waiters are handed the lock FIFO, not by priority.

**S7. Arms, how each is measured, and its mutant.** All in `user/schedtest.c`, all RT threads pinned with `SYS_SCHED_CPU`.
- **D (C4).** A `SCHED_OTHER` spinner and a FIFO thread are pinned to one CPU. Fifty times, the spinner resumes, spins under 1 ms, reads its own `run_ticks` (`SYS_GETPROCS`, DDR-754), and wakes the FIFO thread through a futex. The FIFO thread reads the spinner's `run_ticks` when it runs, then blocks again. Δ = the spinner's ticks between the wake and the FIFO thread's run.
  - With P3, the next tick preempts, so Δ = 1.
  - Mutant C4 (RT threads queued as fair, no tick preemption): the spinner resumes with a full 2-tick quantum, so the FIFO thread waits for it to expire and Δ = 2.
  - **Pass:** at most 5 of 50 wakes have Δ ≥ 2. The 5 allow for a tick that lands between the spinner's read and its wake.
- **E (C5).** Two threads each take RR at priority 10 *before* pinning to one CPU, then each counts loops for 1 s. Taking RR first means the second reaches that CPU as an RR thread, so only rotation lets it run there: a throttle gap goes to `SCHED_OTHER`, not to it. **Pass:** both threads got there (`ready=1`) and both counts are above 0. Mutant C5 (RR never rotates) leaves the second off the CPU until the first exits, so its count is 0. A setup timeout reports `rr_ok=0` rather than ending the probe.
- **F (C6).** Two FIFO threads of priority 10 on one CPU. The first spins for 30 of its own ticks (15 quantum expiries), checking a flag the second sets when it runs, then blocks. **Pass:** the flag is still 0 when the first blocks, and the second runs after. Mutant C6 (FIFO rotates on quantum expiry) sets the flag within 2 ticks.
- **G (C7).** As §4. Mutant C7 removes the throttle check.
- **H (C8, C8b).** A forked child of the probe calls `SYS_SCHED_SETSCHED(FIFO)` and gets `-EPERM`, because fork drops `RES_SCHED` (C8: skip the check). The launcher also starts a second copy of the probe as an agent (`SCHEDAGT`, `is_agent` set, no grant). That copy's own `SYS_SCHED_SETSCHED(FIFO)` returns `-EPERM`, and the probe's `SYS_SCHED_GRANT(<SCHEDAGT pid>)` returns `-EPERM` (C8b: allow the grant to an agent).
- **N (C14).** As §4, measured as arm D: a FIFO-10 spinner, a FIFO-20 thread woken by it, Δ in the spinner's ticks. Mutant C14 reverses the order.
- **P (C16), the DDR-1193 D1 gate.** A probe-only test `kmutex` (`g_pi_test_mx`), reached through two new ops of `SYS_SCHED_CPU` (3: hold it for N of the caller's own ticks of CPU time; 4: lock and unlock it). They return `-ENOSYS` unless `QEMU_PROBES` includes `schedclass`, so no new NSI is spent on a test hook.
  - A `SCHED_OTHER` holder pinned to CPU 2 takes the lock for 30 of its own ticks.
  - A FIFO-10 spinner pinned to CPU 2 then starts.
  - A FIFO-20 waiter pinned to CPU 3 then locks it and times its wait with `CLOCK_MONOTONIC`.
  - With S6 the holder runs at 20 above the spinner and needs about 0.3 s. Mutant C16 (no boost) leaves it only throttle gaps, one tick in 20, so it needs at least 600 ticks, about 6 s.
  - **Pass:** the wait is under 2 s.
  - Syscalls run with interrupts off (`SFMASK`), so op 3's hold loop would never see a tick. It spins in `sti; nop; pause; cli` windows, the `za_hold` idiom (DDR-1201), so the tick charges it, preempts it and boosts it while it holds the lock.

**Thread ids in the arms.** `SYS_GETPID` returns the process id (`tgid`, DDR-1181 P1c), not a thread's own id. Every arm that reads a thread's `run_ticks` takes that thread's id from `SYS_THREAD_CREATE`'s return value, published by its creator. A first draft used `SYS_GETPID`, so arms D, N, F and G read the main thread's ticks, and all four passed on a meaningless number (arm G reported 100 ticks a second for a thread that shared its CPU with a FIFO spinner). That passing run is why each arm is checked against its mutant before it is trusted.

**Vacuity.** Each arm's mutant is a P3 tree with one thing removed, built and run locally before the arm is trusted, then in CI.

**Local results (QEMU TCG, one boot each, not CI):**
- tree with P3: every arm passes (D `late=0 of 50`; N `late=1 of 50`; G `w=5,5,5,5,5`; P `wait_ms=242`).
- C4: red on D (`late=49 of 50`, the predicted Δ = 2), and on F (the fair picker rotates FIFO threads).
- C5: red on E (`n1=0`).
- C6: red on F.
- C7: red on G only. A first arm E took RR after pinning and needed a throttle gap to get there, so C7 went red on E. E now takes RR first (above).
- C8: red on H (the forked child and SCHEDAGT both got FIFO).
- C8b: red on H (the grant to SCHEDAGT succeeded).
- C14: red on N (`abort=1`), and on P (the boost to 20 loses to the spinner at 10 when the order is reversed).
- C16: red on P. Arms D and N rest on the spinner starting each trial with a full quantum: that is checked against the mutant locally before the arm is written, as §4 requires.

**Risks carried from §5.** P3 changes `rq_pop` and `sched_tick`, where OPEN-2 and OPEN-12/13 live. With no RT thread the new code runs nothing but the count check. Every P3 PR runs the whole gate set; a new `[apfreeze]` or hang signature is a stop.

**Correction to Amendment 3, T5 (CodeRabbit on #136, after merge review).** T5 says a one-shot is never armed closer than 50 µs "after the previous timer interrupt". What `hrt_arm` measures from periodic mode is the previous timer **expiry**, `tick_at - 10 ms`. If that tick's interrupt was delivered late by `d`, the next one-shot can expire `50 µs - d` after the handler ran. T5's purpose is the interrupt-rate bound (§5), and that holds: expiries on a CPU stay at least 50 µs apart, and deliveries cannot outnumber expiries. T5 should read "50 µs after the previous timer expiry". Not gated: no gate can delay an interrupt on purpose.
- **Rounding.** `hr_ns_to_counts` truncated, so a count could be up to one LAPIC count short of the target (about 16 ns under QEMU's rate). P3 rounds it **up**. The count starts at the LAPIC write, after `now` was sampled, so the expiry is at or after `now + ns`, at or after the target. This rests on one unmeasured assumption: a count lasts `HR_TICK_NS / g_timer_count`, with `g_timer_count` calibrated against the PIT while `now` comes from the TSC. A rate mismatch moves the floor by that error.
- **Per-window bound.** Spacing bounds expiries to 20 000 a second. One more can be delivered inside a one-second window, latched in IRR from an expiry before it: at most 20 001 deliveries in any one-second window.

**Review fixes on #138.**
- **A grant reached one thread of the process (Codex).** `SYS_SCHED_GRANT` stored the handle in the one `tcb` the pid named. The process's other live threads share its capability table but kept `sched_cap = CAP_NULL`, so they still got `-EPERM`. Fixed: `sched_grant_pid` gives the handle to every live thread whose table is the target's, under `g_sched_lock`. **Not gated:** arm H checks only refused grants (fork, agent); no arm makes a grant succeed.

- **The agent copy was marked an agent after SCHEDTEST could run (CodeRabbit).** SCHEDAGT was loaded after `sched_unblock(SCHEDTEST)`, so arm H's poll could find it, still without `is_agent`, and grant it RT; setting `is_agent` later would not revoke that. Fixed: SCHEDAGT is loaded and marked before SCHEDTEST is unblocked, and unblocked after.
- **A donation was skipped when the owner already outranked the waiter (CodeRabbit).** An owner that later lowered its own priority while holding the lock then ran below its waiter. Fixed: `sched_pi_raise` is always called with the waiter's priority; it keeps the larger donation.
- **Repeated grants filled the target's capability table (CodeRabbit).** Each grant created a new slot. Fixed: a grant whose target already holds a valid handle with those rights reuses it.
- **A thread created while a grant is in flight could keep `CAP_NULL` (CodeRabbit).** It copies its creator's handle, possibly before the grant reaches the creator. Fixed: `SYS_SCHED_SETSCHED` adopts a valid handle from any thread of the same process, under `g_sched_lock`, when its own is missing.
- **The sliding-window throttle bound (CodeRabbit).** Corrected in S4: 95 per aligned second, 96 per sliding second.
- **Not changed: `docs/build_status.md` (Codex).** As on #136: the file has had no entry since October, and reviving or retiring it is an open question to the operator (#45 6948599917).
- None of these is gated: arm H checks refusals only, and no arm lowers an owner's priority while it holds a `kmutex` or repeats a grant.

**Not gated in P3 (stated):** cross-CPU priority; RT priority on steal; a policy change on another thread; a successful grant and its spread to sibling threads; transitive inheritance; priority-ordered `kmutex` hand-off; the head re-queue on preemption by a higher priority (only the throttle case is exercised, by arm F).

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.
