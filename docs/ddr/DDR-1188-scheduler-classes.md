# DDR-1188: Scheduler classes, high-resolution time and pressure stall information (checklist C1.5)

Status: design ahead (#45 comment 6436706603, item 2). No code until C1.4 has
merged (`Needs: C1.4`); the arms below pin threads with `SYS_SCHED_CPU`
(DDR-1186, #91).

## 1. Where the tree is (read from `dev/phase1` = `7981cd4`)

- **Tick.** 100 Hz: `pit_init(100)` on the BSP and `lapic_timer_100hz()` on
  each AP (`kernel/main.c`). Every timeout in the ABI is in milliseconds or
  ticks, so the real resolution is 10 ms.
- **Picker.** Per-CPU FIFO: `struct rq { lock; head, tail; }` in
  `kernel/proc/sched.c`, with `rq_push`/`rq_pop`. There is no priority. The
  vruntime in the same file is a debug shadow (`dbg_vruntime`, DDR-989): it
  observes and never decides. `tcb.weight` exists and no pick reads it.
- **Clocks.** `SYS_CLOCK` (57) and `SYS_TIME` (72) read the RTC (seconds).
  `rdtsc` is read only by debug accounting (`sched_rdtsc`). There is no
  monotonic clock in the ABI and no `nanosleep`.
- **Pressure.** Nothing measures time spent waiting for a CPU, for memory or
  for I/O.

## 2. Scope split (one PR per part, in this order)

| Part | Content | Needs |
|---|---|---|
| P1 | A monotonic clock: TSC calibrated against the PIT at boot, `clock_gettime(CLOCK_MONOTONIC)` in nanoseconds, `nanosleep` | C1.4 |
| P2 | One-shot LAPIC timers (TSC-deadline mode where CPUID has it, else one-shot count), so a timer can fire between ticks | P1 |
| P3 | Scheduling classes: `SCHED_FIFO` and `SCHED_RR` (priority 1-99) ahead of the existing FIFO class (`SCHED_OTHER`); RT throttling | P2 |
| P4 | `SCHED_DEADLINE`: EDF with a constant-bandwidth server and admission control | P3 |
| P5 | Pressure stall information for CPU and I/O; memory once C2.1 can stall on reclaim | P3 |
| P6 | Tickless idle (stop the periodic tick on an idle CPU, wake on the next timer); CPU frequency only where the hardware exposes it | P2 |

The fair class stays the FIFO it is today. Making it a real fair scheduler
(vruntime that decides) is a separate change with its own risk (OPEN-2 timing
lives on this path) and is not in C1.5.

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
| B | P1 | `nanosleep(20 ms)` returns after at least 20 ms by the monotonic clock | C2 | round the sleep down to whole ticks |
| C | P2 | a 2 ms one-shot timer fires before the next periodic tick (count of early wakes > 0 over 50 tries) | C3 | arm the periodic tick only |
| D | P3 | a FIFO thread woken on a CPU busy with a `SCHED_OTHER` spinner runs before the spinner's next tick | C4 | push RT threads to the fair tail |
| E | P3 | two RR threads of equal priority on one CPU both make progress | C5 | RR never rotates (behaves as FIFO) |
| F | P3 | two FIFO threads of equal priority: the second does not run until the first blocks | C6 | FIFO rotates on the tick |
| G | P3 | a `SCHED_OTHER` thread still makes progress beside a FIFO spinner (K2) | C7 | no RT throttling |
| H | P3 | `sched_setscheduler(FIFO)` without `RES_SCHED` returns `-EPERM`; an agent with it also gets `-EPERM` | C8 | skip the capability check |
| I | P4 | admission refuses the deadline thread that would take a CPU over 0.90 (`-EBUSY`) | C9 | no admission check |
| J | P4 | an admitted deadline thread under light load misses no deadline over 100 periods | C10 | EDF ordering ignored (FIFO among deadline threads) |
| K | P5 | CPU `some` pressure is above 0 with two spinners on one CPU, and 0 when that CPU is idle | C11 | never account runnable-wait time |
| L | P6 | an idle CPU takes fewer than 5 ticks in one second | C12 | tick never stopped |

Before each arm is written, I check it for vacuity: it must fail on the tree
without its part. Arm D, for instance, fails on today's FIFO picker because the
woken thread queues behind the spinner.

## 5. Risks

- **OPEN-2 and OPEN-12/13 live on the scheduling path.** P3 changes the picker.
  Every P3 PR runs the whole existing gate set, and any new `[apfreeze]` or
  hang signature is a stop, not a retry.
- **Priority inversion.** A FIFO thread blocked on a kmutex held by a
  `SCHED_OTHER` thread waits behind every other RT thread. P3 does not add
  priority inheritance; this is recorded, and K2's floor bounds the wait.
- **Timer interrupts are a security boundary.** A user-chosen one-shot
  deadline must be clamped (minimum 50 µs between timer interrupts on a CPU),
  or a thread could flood its CPU with interrupts.

## 6. Honesty labels

- Nothing here is built. Each part is **not verified** until its arms are
  green and its mutants are shown red in CI.
- Everything will be emulated (QEMU TCG) only, and **not verified on real
  hardware**; TCG time is not wall time, so the arms assert orderings and wide
  bounds only.
- This record does not close OPEN-1/2/12/13 or U1-U3.
