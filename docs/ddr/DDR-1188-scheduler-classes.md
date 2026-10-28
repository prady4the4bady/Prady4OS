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
| P1 | The time ABI on the existing TSC calibration and vDSO clock: `clock_gettime(CLOCK_MONOTONIC, &ts)` and `clock_gettime(CLOCK_REALTIME, &ts)` (a `struct timespec`, nanosecond resolution), `nanosleep` | C1.4 |
| P2 | One-shot LAPIC timers (TSC-deadline mode where CPUID has it, else one-shot count), so a timer can fire between ticks | P1 |
| P3 | Scheduling classes: `SCHED_FIFO` and `SCHED_RR` (priority 1-99) ahead of the existing vruntime-fair class, which becomes `SCHED_OTHER`; RT throttling | P2 |
| P4 | `SCHED_DEADLINE`: EDF with a constant-bandwidth server and admission control | P3 |
| P5 | Pressure stall information for CPU and I/O; memory once C2.1 can stall on reclaim | P3 |
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

## 4. Gate `smoke-schedclass` (one probe, `user/schedtest.c`; arms by part)

Timing under TCG is not hardware timing, so every arm asserts an ordering or a
bound with a wide margin, never a precise duration.

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| A | P1 | `CLOCK_MONOTONIC` never goes backwards over 10 000 reads on each of two pinned CPUs | C1 | read the TSC without the per-CPU offset |
| B | P1 | `nanosleep(25 ms)` returns after at least 25 ms by the monotonic clock (25 ms is not a whole number of 10 ms ticks, so rounding down to ticks returns at 20 ms) | C2 | round the sleep down to whole ticks |
| C | P2 | a 2 ms one-shot timer fires before the next periodic tick (count of early wakes > 0 over 50 tries) | C3 | arm the periodic tick only |
| D | P3 | a FIFO thread woken on a CPU busy with a `SCHED_OTHER` spinner runs within one tick of its wake, before the spinner's 2-tick quantum ends | C4 | queue RT threads in the fair class (picked by `fair_candidate`, so they wait for the quantum) |
| E | P3 | two RR threads of equal priority on one CPU both make progress | C5 | RR never rotates (behaves as FIFO) |
| F | P3 | two FIFO threads of equal priority: the second does not run until the first blocks, across at least 5 quantum expiries | C6 | FIFO rotates on quantum expiry, as `SCHED_OTHER` does |
| G | P3 | a `SCHED_OTHER` thread still makes progress beside a FIFO spinner (K2) | C7 | no RT throttling |
| H | P3 | `sched_setscheduler(FIFO)` without `RES_SCHED` returns `-EPERM`; an agent with it also gets `-EPERM` | C8 | skip the capability check |
| I | P4 | admission refuses the deadline thread that would take a CPU over 0.90 (`-EBUSY`) | C9 | no admission check |
| J | P4 | two admitted deadline threads on one pinned CPU, A (period 10 ms) and B (period 40 ms). On each of 100 contended releases, B is made runnable first and A second, and both are runnable before the CPU picks: A runs first in at least 95 of the 100, and neither misses a deadline | C10 | EDF ordering ignored (deadline threads picked in arrival order, so B, which arrived first, runs first) |
| K | P5 | CPU `some` pressure is above 0 with two spinners on one CPU, and 0 when that CPU is idle | C11 | never account runnable-wait time |
| L | P6 | an idle CPU takes fewer than 5 ticks in one second | C12 | tick never stopped |
| M | P2 | an unprivileged thread re-arming a 50 µs one-shot as fast as it can gets at most 1 100 early wakes in one second (budget 1 000 plus margin) | C13 | no per-thread budget |

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
  bound. P3 does not add priority inheritance; this is a stated limitation, and
  RT users must not take kmutexes that `SCHED_OTHER` threads hold on a
  latency-critical path.
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
