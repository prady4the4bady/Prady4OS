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
- **A thread created while a grant is in flight could keep `CAP_NULL` (CodeRabbit).** It copies its creator's handle, possibly before the grant reaches the creator. A first fix copied a valid handle from a sibling thread; CodeRabbit then showed the sibling may have exited by the time it is needed. Fixed: when its own copy is missing, `SYS_SCHED_SETSCHED` finds the handle in the process's shared capability table (`cap_find`, a new lookup by resource and rights under the table's lock). The table is the process-shared state, so no sibling is needed. A forked child's copy was revoked, and a revoked slot is never found.
- **Fork revoked only the slot the parent thread named (CodeRabbit).** A thread that missed the handle's copy could fork, and its child kept a live `RES_SCHED` slot that `cap_find` would then hand to it: RT without a grant. Fixed: fork revokes every `RES_SCHED` slot in the child's table, whatever the parent thread holds. Arm H's fork leg checks the common case; the missed-copy case is not gated.
- **The sliding-window throttle bound (CodeRabbit).** Corrected in S4: 95 per aligned second, 96 per sliding second.
- **Not changed: `docs/build_status.md` (Codex).** As on #136: the file has had no entry since October, and reviving or retiring it is an open question to the operator (#45 6948599917).
- None of these is gated: arm H checks refusals only, and no arm lowers an owner's priority while it holds a `kmutex` or repeats a grant.

**Not gated in P3 (stated):** cross-CPU priority; RT priority on steal; a policy change on another thread; a successful grant and its spread to sibling threads; transitive inheritance; priority-ordered `kmutex` hand-off; the head re-queue on preemption by a higher priority (only the throttle case is exercised, by arm F).

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 5 (2026-11-18): P4 design, `SCHED_DEADLINE` in front of the RT classes

Design only; no P4 code exists when this is written. It fills in what §2–§5 leave open for P4. Read from `dev/phase1` at `1c4e6bc` (#138, P3).

**What exists after P3.**
- The one per-CPU queue holds every class. `rt_candidate()` picks the READY entry with the highest effective priority (`max(rt_prio, pi_prio)`). `fair_candidate()` skips entries whose effective priority is above 0. `rq_take` is the fallback.
- `rt_tick()` charges the K2 window, rotates RR and decides preemption on the tick. There is no IPI.
- `sched_unblock()` is the only BLOCKED-to-READY transition (one CAS). A pinned thread is queued only on its CPU (`rq_push` honours `pin_cpu`, DDR-1186).
- Each CPU counts its own ticks in `percpu.ticks`. `g_ticks` is incremented by every CPU's tick, so it is not one CPU's time.

**D1. A deadline thread is the top RT tier.**
- `SCHED_DEADLINE` (policy 3) sets `rt_prio` to 100 (`SCHED_DL_PRIO`), above any FIFO or RR priority (1–99). So every P3 path that tests "effective priority above 0" already treats it as RT:
  - the `g_rt_threads` gate;
  - `fair_candidate` skipping it;
  - the K2 throttle charging its ticks (K2 covers RT and deadline together);
  - the tick preempting a `SCHED_OTHER` or FIFO/RR current thread for it.
- A deadline thread that waits on a `kmutex` donates 100 to the owner (S6, unchanged code). The owner then outranks every FIFO and RR thread, but not the deadline threads. That is one level, as S6. **Not gated.**
- **The pick.** `rt_candidate` first looks at the deadline entries. Among those that are READY and not CBS-throttled (D3), it picks the earliest absolute deadline, and the earliest in list order among equals. Only when there is none does it pick by priority as in P3.

**D2. Time base: the pinned CPU's own tick count.**
- A deadline thread must be pinned (`SYS_SCHED_CPU` op 1) before it takes `SCHED_DEADLINE`, or the call returns `-EINVAL`. While it is a deadline thread, pin and unpin return `-EBUSY`. So it is never stolen (DDR-1186), and its deadlines are counted in its CPU's `percpu.ticks`, which the tick, the pick and the wake all read.
- Runtime and period are converted to ticks conservatively: `runtime_t = runtime / 10 ms`, rounded down; `period_t = period / 10 ms`, rounded up. The thread then never gets more than it asked for.
- **Valid parameters:** 10 ms ≤ runtime ≤ period, and 20 ms ≤ period ≤ 10 s. The deadline equals the period (implicit deadlines), as in most Linux use. Otherwise `-EINVAL`.
- **Why ticks, not nanoseconds.** The budget is charged on the tick (D3) and preemption happens on the tick (S2), so a finer clock buys no precision here. P6's tickless idle must not stop the tick on a CPU with a deadline thread waiting for replenishment (as A1-5 already says for the K2 throttle).

**D3. The constant-bandwidth server (CBS), charged by tick sampling.**
- Each deadline thread has a budget `dl_left` (ticks) and an absolute deadline `dl_deadline` (ticks of its CPU).
- **Charge.** Each tick that finds a deadline thread current takes one tick from its budget. This is the same sampled charging as `run_ticks`. A job shorter than a tick is charged 0 or 1, so the budget is enforced to one tick, not exactly. When the budget reaches 0 the thread is *CBS-throttled* (`dl_thr`) and the tick preempts it to the tail.
- **Replenish.** A CBS-throttled thread becomes eligible again at its deadline. Its new deadline is `now + period_t` and its budget is `runtime_t`. The check is lazy: it runs in the pick and in the tick's scan, under the queue lock, for the entries they look at.
- **Wake (the CBS rule).** In `sched_unblock`, before the push, a deadline thread whose deadline has passed, or whose remaining budget would exceed its bandwidth over the time left (`dl_left * period_t > (dl_deadline - now) * runtime_t`), gets a fresh deadline `now + period_t` and a full budget. Otherwise it keeps both. This is the textbook CBS wake rule. A woken thread is not on any queue and not running, so the wake owns these fields.
- **Work conserving when nothing else can run.** A CBS-throttled entry is skipped by `rt_candidate` and by `fair_candidate`. If nothing else is READY, `rq_take` may still take it, so the CPU does not idle under a READY thread. Linux's hard CBS would idle. The debt is forgiven at the next replenishment. **Stated limit; not gated.**

**D4. Admission (K4).**
- Bandwidth is `runtime/period`, computed from the nanosecond values the caller passed, in 20-bit fixed point. `g_dl_bw[cpu]` sums the bandwidth of the deadline threads pinned to that CPU, under `g_prio_lk`.
- A request that would take the CPU's sum above 0.90 returns `-EBUSY`. A thread already in `SCHED_DEADLINE` that changes its parameters is admitted against the sum without its own old share.
- The share is released when the thread leaves `SCHED_DEADLINE` (to any other policy) and when its `tcb` is freed. That is conservative: an exited thread holds its share until it is reaped.

**D5. The syscall.** NSI 122 `SYS_SCHED_SETSCHED (0, 3, 0, runtime_ns, period_ns)`, the fourth and fifth arguments.
- It takes the same `CAP_SCHED_RT` right as FIFO and RR (K1), through the same check. So arm H's C8 covers that code, but no arm calls `SCHED_DEADLINE` without the right.
- It returns 0, `-EINVAL`, `-EPERM` or `-EBUSY`. No new NSI.
- Fork: the child starts `SCHED_OTHER` with no share, as P3's fork does for RT.
- New `tcb` fields: `dl_runtime`, `dl_period`, `dl_deadline`, `dl_left`, `dl_thr`, `dl_bw`. Each gets an explicit initialiser in `sched_create_state` (CLAUDE.md rule 11).

**D6. The tick.** `rt_tick` gains three things, all inside the existing `g_rt_threads` gate:
- it charges a deadline current thread (D3);
- its scan replenishes the CBS-throttled entries it passes, and leaves the ones still throttled out of `top`, so they cause no preemption;
- it preempts a deadline current thread when a READY, eligible deadline entry has an earlier deadline.
A `SCHED_OTHER` current thread is preempted for an eligible deadline entry by P3's rule (`top > 0`), and a FIFO or RR current thread by `top > me` (100 > 99).

**D7. Arms and mutants.**

| Arm | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|
| I | On one CPU, deadline requests of 50/100 ms and 30/100 ms are admitted, and a third of 20/100 ms (sum 1.00) gets `-EBUSY`. After the first thread returns to `SCHED_OTHER`, the third is admitted | C9 | no admission check (the third gets 0) |
| J | On one CPU, A (10 ms / 30 ms) and B (10 ms / 60 ms) are both admitted. A `SCHED_OTHER` spinner pinned there wakes B, then A, and spins. At the next tick both are READY: A runs first in at least 95 of 100 releases | C10 | EDF ignored: deadline entries are picked in list order, so B, woken first, runs first |
| Q | On one CPU, a deadline spinner (10 ms / 100 ms) beside a `SCHED_OTHER` thread. The `SCHED_OTHER` thread gets at least 30 of its own run ticks in each of 3 one-second windows of `CLOCK_MONOTONIC` | C17 | the budget is never charged, so the spinner is limited only by K2 and the `SCHED_OTHER` thread gets about 5 ticks a second |

- **Arm J's periods differ from §4** (10 and 40 ms there). A wake is seen at the next tick (S2: no IPI), so A can wait up to one tick, 10 ms, before it runs. With a 10 ms period A would miss its deadline on almost every release whatever the order. 30 and 60 ms keep the order test and leave A two ticks of margin. Releases are spaced 70 ms apart, so each wake finds the previous deadline passed and both threads start from a fresh deadline (D3 wake rule).
- **Deadline misses in arm J are reported, not asserted.** A job misses when it finishes later than its release plus its period, by `CLOCK_MONOTONIC`. No mutant here causes misses that the ordering assertion would not already catch, so an assertion on them would be ungated (rule 3). The count is printed as data.
- **Vacuity, read from the code before writing:** on the P3 tree, `SCHED_DEADLINE` is `-EINVAL`, so all three arms fail at their first call. On a P4 tree with C9, C10 or C17, the arm in the table fails for the reason given. Each mutant is built and run locally before the arm is trusted, then in CI on the final head.
- The new sentinels: `PRADYOS_SCHED_I adm_ok=1`, `PRADYOS_SCHED_J edf_ok=1`, `PRADYOS_SCHED_Q cbs_ok=1`.

**Local results (QEMU TCG, one boot each, not CI), on `cc28e9c` (`smoke-shell` 5/5 on `beb4630`, the base merged):**
- the tree with P4: every arm passes, the P3 arms too. I `rc=0,0,-16,0,0`; J `afirst=100 of 100`, `miss_a=0 miss_b=0`; Q `w=90,90,90`.
- C9: red on I only (`rc=0,0,0,0,0`: the third request admitted).
- C10: red on J only (`afirst=0 of 100`: B, woken first, always ran first).
- C17: red on Q only (`w=5,5,5`: the spinner limited only by K2's one tick in 20).
- `kernel.bin` 1,692,042 B, 8,192 B over P3: 11,894 B under the gate for P5 and P6.

**CI failure on the first head (`c185452`), and its fix.**
- **What failed.** `flag-gates (smoke-rcu)` failed in [44271669243](https://github.com/prady4the4bady/Prady4OS/actions/runs/44271669243): `kernel.bin exceeds 1,703,936 B`.
- **Mechanism.** The `RCU_TORTURE=1` build is the largest of the CI builds. Locally on P4 it is 1,704,330 B: `.text` padded to 0x77000, `.rodata` padded to 0x129000, plus 394 B of `.data`. On P3 it was 8,192 B smaller. P4's growth was mostly the embedded `schedtest.elf`, which grew to 26,432 B. That pushed `.rodata` past one more 4 KiB boundary.
- **The number that bounds us.** The default build's headroom (CLAUDE.md) is not the binding number: the `RCU_TORTURE` build is. Measured from clean objects on P4:
  - the default, `ZANCHOR_TEST=1`, `PIPE_TRACE=1` and `BSP_LIVENESS=1` builds land on the same pages;
  - the `RCU_TORTURE=1` build is one or two pages larger.
- **Fix: the two schedclass probes are compiled at `-Os`.** Raising the stage-2 window is a DDR-1183 decision (operator item 5), so it is not taken here; that is the safe option.
  - Every other user probe keeps the default flags.
  - `schedtest.elf` goes from 26,432 B to 16,360 B and `schedagt.elf` from 4,800 B to 4,672 B. Both keep one R+X segment and no writable section.
  - **After the fix** (local, clean objects): the default build is 1,679,754 B (24,182 B under the gate) and the `RCU_TORTURE=1` build is 1,696,138 B (7,798 B under).
  - Every arm is re-run at `-Os` locally, and in CI with every mutant, because the probe's code changed.

**Review fixes on #140 (CodeRabbit, before merge).**
- **A repeated request refilled the budget.** Re-submitting the same parameters subtracted the thread's own share and restored a full budget and a new deadline: unbounded deadline-class time. Leaving to `SCHED_OTHER` and rejoining did the same. Fixed:
  - a thread already in `SCHED_DEADLINE` keeps its deadline, and its budget left is capped at the new runtime;
  - a thread that rejoins before the deadline of the interval it left starts throttled, with no budget until that deadline;
  - that deadline is counted in the ticks of the CPU it was set on (`dl_cpu`, a new `tcb` field). A thread that re-pins before rejoining carries the time left on the old CPU's clock over to the new one. CPUs start ticking at different times, so comparing raw counts across CPUs could refill early (CodeRabbit's second round);
  - leaving still releases the admission share.
- **Admission shares rounded down.** Two 45 000 001/100 000 000 requests each stored 471 859, summing exactly to `DL_BW_MAX`, so both were admitted although they exceed 0.90. Fixed: each share is rounded up. This is conservative: a sum of exactly 0.90 made of shares that do not divide evenly can now be refused. Arm I's 0.5, 0.3 and 0.2 still give `0, 0, -EBUSY`.
- **Equal effective priority.** A `kmutex` owner boosted to 100 by a deadline waiter was not preempted by another eligible deadline entry (`top == me`), although the picker ranks the deadline entry first. Fixed: a non-deadline current thread is preempted whenever an eligible deadline entry waits and K2 allows RT.
- **Charging a thread that has begun to block.** In `switch_wait_offcpu_sched`'s interrupt window, a blocking deadline thread is still `current` and can be woken by another CPU. A tick could then charge the budget `dl_wake` had just refilled. Fixed: only a `THREAD_RUNNING` current thread is charged.
- **`prio` was cast before it was checked.** `SYS_SCHED_SETSCHED` now rejects `policy > 3` and `prio > 99` before narrowing to `uint32_t`.
- **Arm J scores only verified releases.** CodeRabbit raised two ways a release could be scored without both workers competing:
  - a worker not yet parked when it was woken;
  - a tick between the two wakes, which lets B run alone.
  - Each release now checks both. `FUTEX_WAKE` returns how many waiters it woke, and the spinner reads its own `run_ticks` before the first wake and after the second. A release counts only when both wakes returned 1 and the spinner's `run_ticks` did not move. Any other release is retried unscored, up to 300 tries for 100 scored releases.
  - A 70 ms quiet gap precedes every release, so both previous deadlines have passed.
  - Each futex word is 0 (idle), 1 (a release) or 2 (shut down). A worker consumes a release by a 1 → 0 compare-and-exchange, so it can never overwrite the 2 the spinner publishes before its final wake. A worker that has not reached `FUTEX_WAIT` then sees a value mismatch instead of sleeping.
  - Locally the arm scored 100 of 100 in 112 releases: 12 were discarded as unverified. C10 scored 0 of 100 in 102.
- **None of these is gated:** no arm repeats a request, rejoins, sums shares near 0.90 by rounding, boosts a `kmutex` owner from a deadline waiter, or blocks during the switch window.

**Not gated in P4 (stated):**
- deadline inheritance through a `kmutex` (D1);
- the work-conserving run of a CBS-throttled thread (D3);
- the CBS wake rule's "keep the old deadline" branch (arm J's releases always start fresh);
- `-EBUSY` on pin and unpin, and `-EINVAL` for an unpinned or out-of-range request;
- share release on `tcb` free;
- preemption of one deadline thread by another with an earlier deadline (arm J's two threads become READY together).

**Risks carried from §5.** P4 adds work to `rq_pop` and `rt_tick` only behind `g_rt_threads`, and one policy test in `sched_unblock`. With no deadline thread the deadline code runs nothing. Every P4 PR runs the whole gate set; a new `[apfreeze]` or hang signature is a stop.

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 6 (2026-11-18): P5 design, pressure stall information for CPU and I/O

Design only; no P5 code exists when this is written. It is read from P4's head (`c15-p4`, #140), on which P5 is stacked because both touch `sched_tick`. P5 opens as a PR only after P4 merges.

**What exists.**
- Each CPU ticks at 100 Hz and counts its own ticks (`percpu.ticks`). The run queue `g_rq[cpu]` is a list with a lock and no count.
- Every block device is virtio-blk in the gate VMs (`boot_test.sh`). A virtio-blk request's issuer waits for its completion in `submit()` (`virtio_blk.c`), in a `sched_block_timeout` loop on `req[s].done`. There is no block cache, so every `blk_read` reaches the device.

**P1. Definition.** As §2: `some` is the number of ticks, out of the last 100 sampled, in which at least one thread was stalled. That is a percentage of the last second at 100 Hz: a plain sliding window with no decay.
- **CPU, per CPU.** A tick on CPU `c` is *stalled* when `g_rq[c]` holds a READY entry, a thread runnable on `c` but not running. Two kinds of READY entry do not count:
  - **The idle thread**, which is never queued (`schedule()` re-queues `prev` only when it is not idle). The check is defensive.
  - **A thread queued by its own `yield()`.** It offered the CPU away, so it is not being denied it. A new `tcb` field `psi_yld` marks it. `yield()` sets the mark with interrupts already off (`schedule_yield`), and `schedule_locked` consumes it on entry and restores it only on that call's own re-queue, so a timer preemption never carries it (CodeRabbit on #145).
  - **Why the yield exclusion.** A first draft counted every READY entry. In the gate VM, every CPU then read 100 before any load (`pre 100 100 100 100`). The queues hold busy-polling threads that never stop: SURFTEST, INPUTTST, AETHERD, PRISM, INIT and the reaper. Each is dispatched about 20 000 times a second against about 25 run ticks a second, a `yield()` loop. With them counted, CPU `some` is pinned at 100 on every CPU of this OS and measures nothing.
  - **Deviation from Linux, stated.** Linux counts a yielding task as runnable. Here a CPU stall means a thread that wants the CPU and was not given it: woken and not yet run, or preempted.
  - With the exclusion, the same VM reads 0 on every CPU without load (`base=0,0,100,0`: CPU 2 still holds the last second of arm Q). The tick takes the queue lock and stops at the first READY entry; most queues hold a few entries.
- **I/O, system-wide.** A tick is *stalled* when at least one thread is waiting for a virtio-blk **read** (`VIRTIO_BLK_T_IN`) to complete. A counter `g_psi_io_wait` is raised before the completion wait in `submit()`. It is lowered once, by whichever comes first: the completion (`reap_head`) or the wait's timeout. A per-request flag `req[s].psi`, under `compl_lock`, makes it exactly once, so the stall ends when the disk finishes, not when the issuer runs again (CodeRabbit on #145). Only CPU 0's tick samples the I/O window, so its time base is one CPU's ticks. P6 must keep CPU 0 ticking, or move the sample.
- Writes and flushes are not I/O pressure in P5. §2 names reads only.
- **Memory pressure** waits for C2.1 (§2).

**P2. The window.** Each window is a 128-bit ring (two `uint64_t`) indexed by `ticks % 100`, plus a count. Each tick clears the old bit at its index, sets the new one, and adjusts the count. Per-CPU windows are written only by their own CPU's tick. The I/O window is written only by CPU 0's. A reader reads the count with one relaxed load: one tick stale at worst.

**P3. The interface. NSI 124 `SYS_PSI (res, cpu)`.**
- `res` 0 = CPU (`cpu` is the index), `res` 1 = I/O (`cpu` ignored).
- It returns 0–100, the stalled ticks in the last 100, or `-EINVAL` (bad `res` or `cpu`).
- No capability: pressure is a count, like Linux's world-readable `/proc/pressure`, and it names no thread.

**P4. The probe's I/O load: `SYS_SCHED_CPU` op 5, probe-only** (`g_sched_probe`, as ops 3 and 4; `-ENOSYS` otherwise). Arm O reads 16 MiB: a 4 MiB read finished in about 20 ms under TCG and raised the window by only 2 ticks. It reads `n` MiB (1–16) from the start of the first block device that holds that many, in 64 KiB requests into one 64 KiB buffer (`pmm_alloc_pages(4)`). It returns 0, or the first error. It is read-only, so whichever disk it picks is safe. A first draft used device 0, the boot image, which is 2 MiB, and op 5 returned `-EINVAL`. No new NSI is spent on a test hook.

**P5. Arms and mutants.** Mutants land on different arms (rule 3), so §4's arm K is split into its two halves.

| Arm | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|
| K | two `SCHED_OTHER` spinners pinned to CPU 2 for 1.5 s: CPU 2's `some` is at least 50 | C11 | the CPU stall bit is never set (always 0) |
| K0 | 1.5 s after both spinners exit: CPU 2's `some` is at most 5 | C11b | the window never forgets: an old bit is not cleared (it stays near 100) |
| O | a thread reads 16 MiB through op 5: the I/O `some` the main thread samples while the read runs, or within the 100-tick window after it, is above 0 at least once, and 2 s after the read it is 0 | C15 | `g_psi_io_wait` is never raised (always 0) |

- **Why K asserts ≥ 50 and not §4's "above 0".** Two spinners on one CPU leave one READY on almost every tick, so the value is near 100. A partial accounting bug would still pass "above 0".
- **Why K0 allows 5 and not 0.** Unpinned threads (the shell, kernel workers) may briefly queue on CPU 2. A tick where two of them are runnable together is stalled, rightly. §4's "0 when that CPU is idle" assumed an idle CPU, which a gate VM does not have.
- **O's "back to 0"** shares the window code with K0, so C11b also turns it red. That is a second witness, not the arm's own mutant.
- **Vacuity, read from the code before writing:** on the P4 tree `SYS_PSI` does not exist (`-ENOSYS`), so all three arms fail at their first read. Each mutant is built and run locally before its arm is trusted, then in CI on the final head.
- The new sentinels: `PRADYOS_SCHED_K some_ok=1`, `PRADYOS_SCHED_K0 idle_ok=1`, `PRADYOS_SCHED_O io_ok=1`.

**Local results (QEMU TCG, one boot each, not CI):**
- the tree with P5: every arm passes. K `base=0,0,100,0 load=100 idle=0`; O `peak=8 after=0`.
- C11: red on K only (`load=0`).
- C11b: red on K0 (`idle=100`) and on O (`after=100`), the second witness predicted above.
- C15: red on O only (`peak=0`).
- `kernel.bin`: 1,683,850 B by default and 1,700,234 B with `RCU_TORTURE=1` (3,702 B under the gate). That counts the probe at `-Os`, from P4's CI fix. P6 has under 4 KB left in the binding build.
- The first draft failed K0 (`idle=67`, then `idle=74` after excluding the idle thread) for the polling-thread reason above, and O (`rc=-22`, device 0 too small). Both were measured, named and fixed before the mutants were run.

**Two P4 review fixes carried here.** CodeRabbit raised both on #140's head `1227ff6` as minor. The reply on #140 (6977372773) moved them to P5, which changes the same code.
- **`rt_tick`: a spent budget with no replacement.** A deadline thread whose budget ran out was preempted even with nothing else runnable. The picker then handed the CPU back to it, after an idle detour:
  - on an AP, one extra switch, because the idle loop re-checks the queue;
  - on the BSP, a halt for the rest of the tick, because its idle path does not re-check.
  The thread now yields the CPU on a spent budget only when some thread could run here instead: READY on its own queue, or on a queue this CPU may steal from (`rq_has_ready()`). A first version looked at the local queue only. CodeRabbit on #145 showed that this kept a remote READY thread waiting, because `schedule_locked` steals (`rq_steal`) before it re-queues `prev`. With stealable work counted, the guard skips the preemption only when `rq_has_ready()` sees no queue head this CPU may take, and there it removes the detour. `rq_has_ready()` is a hint, not an exact test (CodeRabbit on #145): it skips a queue whose head is pinned to another CPU, reads a busy queue lock as ready, and can see stale state. So a runnable entry behind a pinned-away head can still wait a tick, and a busy lock costs one extra switch. The picker was already work-conserving: it falls back to the throttled thread when nothing else can run.
- **`rq_pop`: the fallback under K2.** On a K2-throttled CPU with no `SCHED_OTHER` entry READY, `rq_pop` took the queue head. The head can be a CBS-throttled deadline entry while an eligible one waits behind it. In that case `rq_pop` now ranks the queue with `rt_candidate`, which skips throttled deadline entries, before falling back to the head.
- **Neither has an arm.** The first needs a deadline thread pinned to CPU 0. The second needs K2 throttling with no `SCHED_OTHER` work and two deadline entries in that order. No arm creates either. Arm Q still preempts on a spent budget, because its `SCHED_OTHER` spinner shares the CPU. Mutant C17 (no charge) is unaffected.

**Review fixes on #145 (CodeRabbit), before merge:**
- **Op 5 and a timed-out read.** `submit()` returns `-EIO` on a data-wait timeout and leaves the descriptor chain with the device (DDR-1160). Op 5 freed its 64 KiB buffer whatever the result, so a late DMA write could land in reused memory. It now keeps the pages after `-EIO`: a deliberate probe-only leak. `sys_disk_list` (NSI 104) frees its sector page the same way after a possibly timed-out read. That is outside P5 and is recorded as a separate task.
- **A busy queue lock.** `psi_tick` read the head pointer alone when the trylock failed, which counted a queue holding only a yielder as stalled. It now repeats the last sample taken under the lock. This is an intended approximation: each tick that finds the lock busy may be stale in either direction, consecutive busy ticks repeat the same sample, and the error is one window bit per busy tick (CodeRabbit on #145). An exact, lock-free count of eligible waiters would add state to every queue push and pop.
- **The yield mark** and **the I/O stall's end**, as described in P1 above.
- **Arm O's wording.** The reader can finish before the main thread's first poll, so the arm may see the read's stall in the 100-tick window just after it. That is the window's own meaning. C15 still turns O red at any timing, because then nothing raises the count. The assertion now says so.

**Not gated in P5 (stated):**
- the yield exclusion (`psi_yld`): no arm runs a yielder beside a waiting thread; the I/O window's sampling only on CPU 0, and the exclusion of writes and flushes; the one-tick-stale read; `-EINVAL` paths; I/O through a non-virtio device (none exists in the gate VMs).
- the two carried P4 fixes above;
- the review fixes on #145: op 5 keeping its buffer after a timeout, the busy-lock sample, the yield mark's timing, and the I/O stall ending at completion. No arm creates a timeout, a busy queue lock with only a yielder queued, a tick inside `yield()`, or a delayed issuer.

**Risk.** P5 adds one queue-lock walk to every tick, stopping at the first READY entry, and two atomics per virtio-blk read. The walk is on the scheduling path, where OPEN-2 and OPEN-12/13 live. Every P5 PR runs the whole gate set; a new `[apfreeze]` or hang signature is a stop.

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 7 (2026-11-18): P6 design, tickless idle on the APs

Written as design before any P6 code, read from P5's head (`c15-p5` at `b8bead4`). The P6 code is in the same PR; where the code differs from the design, "As built" below says so and why.

**What exists.**
- **Who owns the clock.** Every CPU's LAPIC timer ticks at 100 Hz on vector 48 (`lapic.c`). The BSP's tick runs `timer_tick` (`idt.c`): `g_ticks`, the vDSO wall clock (10 ms added per tick), the lwIP timers, the heartbeat and its watchdogs. An AP's tick runs only `sched_tick` for that CPU, plus ring-3 signal delivery.
- **How an AP sleeps.** The AP idle loop (`sched_ap_enter`) runs `schedule()`, sets `pc->idle`, re-checks `rq_has_ready()`, then `sti; hlt`. `rq_has_ready()` is true while ANY queue has a head that this CPU may take.
- **How an AP is woken.** `sched_unblock` pushes the thread on the waker's own CPU and sends the wake IPI (vector 49) to the first idle CPU other than itself (only `pin_cpu` for a pinned thread). `smp_resched_one` declines the BSP, so the BSP is woken only by its timer. The code says so: "the timer remains the backstop if none is (visibly) idle".
- **Who reads an AP's `percpu.ticks`:**
  - the K2 window (`ticks / RT_WIN_TICKS`) and the PSI ring (`ticks % 100`), as a clock;
  - `dl_now`, the deadline clock of a thread pinned there (A5), as a clock;
  - RCU (`rcu.c`), as the quiescent-state counter: a grace period waits until every other CPU's counter has moved. Its stall clock is `RCU_STALL_TICKS` = 1 000;
  - the BSP heartbeat's AP freeze check (`idt.c`): every 500 ticks it compares each AP's counter with the last heartbeat. An unchanged counter prints `[apfreeze]`, which is in `GLOBAL_FORBIDDEN`. That means every gate fails;
  - the boot check `smppreempt_proof` (`main.c`): an AP's counter must move within 30 BSP ticks, or it prints `ap preempt FAIL`, which is in `GLOBAL_FORBIDDEN`.
- **The gate VM never idles.** Six unpinned busy-polling threads (A6 P1) keep a READY entry on some queue almost always. With `rq_has_ready()` reading every queue, no AP reaches `hlt` in an ordinary gate run. §4's arm L ("an idle CPU takes fewer than 5 ticks in one second") cannot be met as written, so this amendment restates it (T6).

**Decisions (the safe option in each case; operator order 6761846242).**
- **T1. APs only.** The BSP's tick is the global clock (`g_ticks`, the vDSO wall time, the heartbeat). It also samples the PSI I/O window (A6), and nothing but its timer wakes it. It keeps its periodic tick.
- **T2. Entry.** In the AP idle loop, after `rq_has_ready()` reads false and with interrupts off, the AP stops its tick only when all of these hold:
  - (a) `g_nohz` is set. It is set after the boot self-checks (`smpresched_proof`), so `smppreempt_proof` still measures a ticking AP;
  - (b) no P2 one-shot slot is pending on this CPU (`npend == 0`). P2 owns the timer while a sleeper waits;
  - (c) nothing is READY that this CPU may take, which `rq_has_ready()` already established.
  - A K2-throttled RT thread or a CBS-throttled deadline thread is still a queued READY entry, so an AP with one never idles. A1-5's replenishment rule and A5's rule therefore hold by construction, with no extra timer.
- **T3. A bounded stop.** The AP programs one LAPIC one-shot of `NOHZ_MAX` = 100 ticks (1 s) and halts. It never stops the tick indefinitely:
  - A push that sends no kick (a preempted thread re-queued on a busy CPU, a steal imbalance) is still noticed within 1 s. The backstop degrades; it does not vanish.
  - The freeze check (500 ticks) and the RCU stall clock (1 000) keep a margin of at least 5×.
- **T4. Exit and catch-up.** Any interrupt ends the `hlt`: the wake IPI, a device, or the one-shot. With interrupts off, the idle loop then:
  - computes `k`, the whole ticks elapsed since entry, from the TSC (`ktime_mono_ns`);
  - adds `k` to `percpu.ticks`, so the K2 window, the deadline clock, RCU and the freeze check see time pass;
  - records `min(k, 100)` unstalled ticks in the CPU's PSI window (an idle CPU is not stalled);
  - restores the periodic timer.
  - A new P2 phase, `HR_NOHZ`, makes `hrt_irq` treat the one-shot's own interrupt as no tick, so `sched_tick` does not run for it. A late periodic interrupt racing the restore counts as one extra tick, as P2's `hr_irr48` case already does.
- **T5. RCU: an extended quiescent state.**
  - **Entry:** the AP adds 1 to `percpu.ticks` (sequentially consistent), then sets `pc->nohz`.
  - **Exit:** it advances `ticks` by `k` (T4), then clears `pc->nohz`.
  - **`rcu.c`:** the snapshot skips a CPU whose `nohz` is set, because an idle CPU holds no read section. A CPU that enters `nohz` after the snapshot has already moved its counter.
  - **Why it is needed:** without it, every `synchronize_rcu` waits up to 1 s for a sleeping AP. The tcb and kernel-stack frees that DDR-1207 measured on #135 would wait with it.
- **T6. The gate's idle CPU: a probe-only isolate op.** `SYS_SCHED_CPU` op 6 `(cpu, on)` exists only under `g_sched_probe` (as ops 3–5) and answers `-ENOSYS` otherwise. While a CPU is isolated:
  - `rq_push` sends an unpinned thread aimed at it to the BSP instead. That covers every push path, including `schedule()` re-queuing a preempted unpinned thread;
  - `steal_pass` on that CPU takes nothing;
  - the unblock kick skips it;
  - turning isolation on moves its queued unpinned entries to the BSP.
  - An isolated CPU therefore runs only threads pinned to it. This is a test hook, not an `isolcpus` feature, and it is not claimed as one.
- **T7. Counting interrupts.** A per-CPU count of vector-48 interrupts, `tirq`, read by op 7 `(cpu)`. Arm L counts interrupts, not `ticks`, because T4 advances `ticks` without an interrupt.
- **T8. CPU frequency (K5).** At boot P6 reads CPUID leaf 6 (HWP, EAX bit 7) and leaf 1 (EIST, ECX bit 7), and prints `[cpufreq] hwp=<0|1> eist=<0|1>`. There is no frequency control. Under TCG both are expected to read 0; this is to be measured. Labelled not verified on real hardware.

**Arms and mutants** (one mutant per arm, rule 3; §4's single arm L is split three ways):

| Arm | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|
| L | CPU 3 isolated, nothing pinned to it, 1 s to drain: CPU 3 takes fewer than 5 timer interrupts in the next second | C12 | the tick is never stopped (about 100) |
| L1 | a thread pinned to isolated CPU 3 sleeps 20.1 ms with `nanosleep`, 50 times: at least 40 of the 50 end less than 2 ms late, and none more than 2 ticks (20 ms) late (redefined after the first CI run, below) | C12b | the stop ignores the pending sleeper's deadline (T2 b as built), so the sleep ends on nanosleep's tick backstop, about one tick late |
| L2 | with CPU 3 isolated and idle, 20 grace periods (op 8, a probe-only `synchronize_rcu` that returns its duration) each finish within 50 ms | C12c | no extended quiescent state: each grace period waits for CPU 3 (up to 1 s) |

- **Vacuity, read from the code before writing:** on the P5 tree ops 6–8 answer `-ENOSYS`, so all three arms fail at their first call. C12 leaves L1 and L2 green (the tick keeps running), C12b leaves L and L2 green (no slot is pending in either), and C12c leaves L and L1 green. So each mutant is red on its own arm only, which is what rule 3 asks.
- New sentinels: `PRADYOS_SCHED_L nohz_ok=1`, `PRADYOS_SCHED_L1 wake_ok=1`, `PRADYOS_SCHED_L2 eqs_ok=1`.

**Size.** Designed against the 52-chunk gate (1,703,936 B), with a window raise as the stated remedy. That raise came first, for DDR-1208 (DDR-1183 Amendment 1, #147: 53 chunks, 1,736,704 B). Measured on the P6 code, built locally:
- default `kernel.bin` 1,687,946 B, unchanged by P6: the code fits the existing page slack;
- `RCU_TORTURE=1` 1,704,330 B, also unchanged, 32,374 B under the gate.
- CI prints the figures of record.

**As built (P6 code, read against T1–T8).**
- **Entry keeps the wake race closed.** The idle loop clears IF before it sets `pc->idle` and re-checks `rq_has_ready()`, and keeps it clear through the stop or its refusal until `sti; hlt`. A wake IPI sent after the re-check stays pending and breaks the `hlt`. As first written, IF was cleared only after the re-check, so an IPI taken in between was consumed and a stopped tick could sleep the full bound with work queued (CodeRabbit on #150; before P6 the same window cost at most one tick). The same check now also reads the `smp_run_on` mailbox: a job posted after the loop's mailbox read, whose IPI was taken before the `cli`, is work, so the stop is not entered (CodeRabbit on #150, third round; the same one-tick-to-1 s widening). Not gated.
- **The deadline clock during a stop.** A CBS wake from another CPU (`sched_unblock`, then `dl_wake`) reads `dl_now`, the pinned CPU's `ticks`, which a stop freezes. While `nohz` is set, `cpu_now` (used by `dl_now`, and by the deadline rejoin's read of the CPU it left) returns the value the exit will catch up to: the count before entry plus `max(k, 1)`, with `k` from CLOCK_MONOTONIC since entry. The exit moves `ticks` before it clears `nohz`, so the two readings agree. The entry writes the base and start time inside an odd generation count, and a reader retries on a change, so it never mixes two stops (CodeRabbit on #150, both rounds). The exit runs inside the generation too, and the reader checks it again after its time sample (CodeRabbit on #150, third round): a reader that raced an exit retries, so the clock never reads one tick ahead and then steps back. Not gated: no arm runs a deadline thread across a stop.
- **T2 (b) revised: a pending sleeper does not prevent the stop.** As designed, the stop refused while any P2 slot was pending. Measured on the first local run, arm L failed with 100 interrupts in its second: every stop was refused on `npend`, because `hrt_arm` takes a slot for every `nanosleep`, so some sleeper almost always holds one. As built:
  - `hrt_nohz_enter` refuses only when the timer is in a one-shot phase (P2 owns it), a tick is latched, or the earliest deadline is inside the next tick.
  - Otherwise the one-shot counts to the earlier of the earliest deadline and the 1 s bound.
  - While stopped, the stop's interrupt expires due sleepers (`hr_expire`), then answers 0.
  - On exit, a sleeper due within a tick gets P2's one-shot, as `hrt_irq`'s tick path does. Without that, a LAPIC interrupt slightly early by the TSC left the sleeper a whole tick late (L1 measured 10 ms, then 0.4 ms with the one-shot).
  - Mutant C12b now ignores the sleeper's deadline (the count always runs to the bound).
- **L1 as first built did not see T2 (b); C12b survived.** On `d83db5b`, C12b's run was green (run [44676499866](https://github.com/prady4the4bady/Prady4OS/actions/runs/44676499866): `L1 late_us=4844 valid=50`, `wake_ok=1`). The line above, "the L1 sleeper waits for the bound", was wrong. The mechanism, read from the code and then measured:
  - `nanosleep` keeps a tick backstop: each block is `rem / 10 ms + 1` ticks of `g_ticks` (`sched_block_timeout`), and `sched_timed_sweep` runs in every ticking CPU's `sched_tick`. The BSP never stops, so the backstop is always serviced.
  - For a 20 ms sleep, `rem` is just under 20 ms, so the backstop is 2 ticks. It fires at or before the deadline, whatever the phase.
  - Its wake kicks CPU 3 out of any stop. The exit then finds the sleeper due within a tick and arms P2's one-shot, which ends the sleep on time. So the wake was precise whether or not the stop counted to the deadline.
  - Measured locally with a diagnostic print (not committed), across L1's 50 sleeps on CPU 3: correct code `stops=50 refused=99 irqs=100`, 50 of 50 under 1 ms late; C12b `stops=49 refused=101 irqs=100`, 44 of 50 under 1 ms late. Same counts, nearly the same timing: the stop's own count was not what ended the sleeps.
  - **Redefined.** The sleep is 20.1 ms. The backstop is then 3 ticks and lands after the deadline, unless the sleep blocks in the last 0.1 ms before a BSP tick (then it is at most 0.1 ms early, and the exit's one-shot makes that wake precise). Without T2 (b) the sleep ends on the backstop. With it, the stop's interrupt ends it at the deadline. The arm counts the samples that end less than 2 ms late (`fine=`) and needs 40 of 50; the old bound, none more than 2 ticks late, stays.
    - **Where the backstop lands (CodeRabbit on #152).** A sleep that begins φ after a BSP tick (0 ≤ φ < 10 ms) has its third tick 30 − φ ms after it begins. A backstop wake is therefore 9.9 − φ + λ ms late, where λ is its wake latency. It counts as fine only if φ − λ exceeds 7.9 ms (CodeRabbit on #152, a sign corrected): a sleep that begins 8.5 ms after a tick ends about 1.4 ms late with no latency, while one that begins 8 ms after a tick with 1 ms of latency ends 2.9 ms late.
      - **Assumption (CodeRabbit on #152, after merge).** The formula assumes no BSP tick lands between the start of the sleep and the moment `sched_block_timeout` reads `g_ticks` (it registers `block_deadline = g_ticks + ticks`). If one lands there, the backstop counts from the newer `g_ticks` and fires one tick later: 19.9 − φ + λ ms late. That only makes a backstop sample later, never fine. A tick after the read changes nothing.
      - **Which wake ends the sleep.** With T2 (b) present, the armed one-shot normally ends the sleep at the deadline. Without it (C12b), the backstop decides a sample when it fires at or after the deadline. An early backstop leads to the two cases below in either build (CodeRabbit on #155).
      - **An early backstop (CodeRabbit on #153 and #155).** When φ is above 9.9 ms, the third tick comes up to 0.1 ms before the 20.1 ms deadline, so the backstop can fire while the one-shot is armed.
        - If the thread runs again before the deadline, the loop rechecks it, blocks again, and the one-shot ends the sleep.
        - If the thread first runs after the deadline, the loop returns without blocking. The backstop's wake plus the scheduling latency λ then decides that sample, and the armed one-shot cannot change it: a kick does not move a thread that is already READY. By the formula above, that sample is at most λ late.
    - **Why the mutant still fails.** The samples run back to back. Under C12b each one begins just after the previous one's backstop wake, so φ is about the previous wake's latency, φ − λ is near 0, and the sample ends about 9.9 ms late. CI measured `fine=6` on C12b-v3.
    - **The 0.1 ms margin.** `sys_nanosleep` (`kernel/syscall/sys_proc.c`) sets the deadline, arms the one-shot, takes the sleep lock, and only then computes `rem` and the tick count. 3 ticks need `rem` ≥ 20 ms there, which leaves 0.1 ms for that window. A longer delay gives a 2-tick backstop, which can fire before the deadline. The loop then rechecks the deadline and blocks again, so the sleep never returns early.
    - **40 leaves room on both sides.** The correct code measured 50 of 50 under 1 ms in the diagnostic run of the old arm, and 50 of 50 under 2 ms on the redefined arm (worst 1,977 µs locally; 49 of 50 in CI, worst 2,265 µs) (CodeRabbit on #151).
  - Measured on the redefined arm, local, this head: correct code `fine=50` of 50, worst 1,977 µs (`wake_ok=1`); C12b `fine=1`, worst 10,943 µs (`wake_ok=0`, red), with L and L2 green under C12b.
  - **CI of record** on the merged head `304a781` (#150, merged as `0ae6251`): green, run [44687523048](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687523048). C12 red on L only (run [44687627569](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687627569)), C12b red on L1 only (run [44687624768](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687624768)), C12c red on L2 only (run [44691904269](https://github.com/prady4the4bady/Prady4OS/actions/runs/44691904269)).
- **The clock moves by `max(k, 1)` in all.** T5's entry adds 1, so the exit adds `k - 1` (T4 said `k`; that would count every stop one tick long). The PSI window records the same ticks, at most 100, as unstalled.
- **RCU readers.** The only RCU reader on an AP today is `sched_timed_sweep`, inside `sched_tick`, and `hrt_irq` answers 0 while the tick is stopped. So no reader runs on a CPU with `nohz` set, which is what T5 relies on. A future reader in an interrupt path must end the stop first; this is stated, not gated.
- **Isolation (T6)** is a per-CPU flag read by `rq_push` (unpinned threads go to the BSP), `rq_steal` and `rq_has_ready` (the CPU takes nothing but its own queue), and the unblock kick. The BSP is found by `is_bsp`, not assumed to be index 0, and cannot be isolated (op 6 answers `-EINVAL`). Isolating drains the CPU's unpinned entries until none is left. A producer that read the flag just before it was set can still queue one unpinned thread there after the drain; it runs once and is re-queued to the BSP at its next `rq_push` (CodeRabbit on #150, not closed: a lock-held check in every push for a test hook).
- **Op 8** opens interrupts around `synchronize_rcu` (syscalls run with IF clear; rcu.h rule 2), as op 3 does, and returns the grace period's length in microseconds.
- **`[cpufreq]`** is printed once, after the boot proofs, with `nohz=1`.

**Not gated in P6 (stated):**
- the 1 s bound as a backstop: no arm loses a kick on purpose;
- the catch-up of the K2 window and the deadline clock across a stop: no arm runs an RT or deadline thread on an isolated CPU across one;
- the PSI catch-up;
- the BSP, which never stops;
- the CPUID report;
- the isolate op's drain of queued entries (L's 1 s drain covers it only indirectly).

**Risk.** The AP idle path is where OPEN-2, OPEN-12/13 and the `[apfreeze]` signatures live. In ordinary gate runs no AP reaches `hlt` (the pollers), so the stop engages almost only on the isolated CPU of arms L–L2. That is both the safety margin and a coverage gap: a real idle system exercises T2–T5 far more than CI does. Every P6 PR runs the whole gate set. A new `[apfreeze]`, `[rcu] stall` or hang signature is a stop.

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.
