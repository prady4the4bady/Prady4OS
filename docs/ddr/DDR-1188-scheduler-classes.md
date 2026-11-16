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
  - **Not changed here:** `FUTEX_WAIT` (`kernel/proc/futex.c`) has the same gap for default-terminate signals. It predates this PR and is recorded as a follow-up, so the futex path is not widened without its own gate.

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

§4's arm C says "a 2 ms one-shot timer fires before the next periodic tick (count of early wakes > 0 over 50 tries)". By elapsed time alone it is vacuous: with ticks only, a 2 ms sleep already ends in under 10 ms about 8 times in 10 (above).

- **The arm.** A thread pinned to a CPU with no other load runs 50 tries of `nanosleep(2 ms)`. Around each try it reads the vDSO's `wall_time_ns`, which the BSP advances once per tick, in the same handler that advances `g_ticks`.
- **A try is early when** `wall_time_ns` did not change across the sleep **and** the sleep lasted at least 2 ms by `CLOCK_MONOTONIC`.
- **Pass:** at least 20 of 50 tries are early, and the pin succeeded. Sentinel: `PRADYOS_SCHED_C early_ok=1 pin_ok=1`, with the count on a `[sched] C` line.
- **Why ticks alone cannot produce an early try.** A tick-timed wait ends only after `g_ticks` advanced, and `timer_tick` advances `wall_time_ns` three lines later. A sweep on another CPU could land between those two stores. That is a window of a few instructions, which is why the bar is 20 and not "more than 0".
- **Why 20 and not more.** A try that happens to span a tick is not early even with one-shots. A 2 ms sleep spans a tick about 2 times in 10, and TCG adds latency.
- **Vacuity is checked on the baseline before the arm is written:** the arm's count on the tree without T2 must be far below 20. The number goes in the PR.
- **Mutant C3** ("arm the periodic tick only"): the arming step never switches the LVT. Expected: red on arm C only.

### Arm M

- **The arm.** One thread first sleeps 1.05 s. That sleep takes one one-shot when it begins, so more than a window passes with no new one. The thread then calls `nanosleep(50 µs)` in a loop for 1 s of `CLOCK_MONOTONIC` and counts the calls that returned. The loop's first sleep opens a new window, and the whole second lies inside it.
- **Pass:** the count is at most 1 100. With T4, at most 1 000 calls get a one-shot; each of the rest ends on a tick. The 1 000 one-shot sleeps take at least 50 ms together, so at most 96 ticks are left in the second. Sentinel: `PRADYOS_SCHED_M capped_ok=1`, with the count on a `[sched] M` line.
- **Vacuity.** The arm is only meaningful if the unbudgeted rate is above 1 100 per second under TCG. That is measured with C13 before the arm is accepted. If TCG cannot reach it, the arm is redesigned, not weakened.
- **A lower bound too:** the count must be at least 500. Otherwise a kernel that arms no one-shot at all (about 100 per second) would pass arm M.
- **Mutant C13** ("no per-thread budget"): the budget check is removed. Expected: red on arm M only.

### Not gated in P2

- T5's 50 µs spacing. No arm separates it from T4. A mutant that drops it would be recorded as a survivor.
- The table-full path (more than 8 pending sleeps on one CPU).
- A sleeper that resumes on another CPU and cancels there.
- The size of the tick delay described above.
- TSC-deadline mode (not built).

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1-U3.
