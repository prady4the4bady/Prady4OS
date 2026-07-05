# DDR-SMP-3c-cap-3 — per-AP LAPIC-timer preemption (ADR-031 D4)

> DDR before code. cap-2b let APs run ring threads but only **cooperatively**
> (an AP reschedules on block/yield/exit/idle). A CPU-bound kernel thread on an
> AP runs forever. cap-3 arms each AP's own LAPIC timer so its vector-48 tick
> drives `sched_tick`→`schedule()` — real preemption on every CPU.

## Decisions
- **D1 — reuse the BSP's calibrated count.** `lapic_timer_100hz` (BSP)
  calibrates the APIC-timer count-per-10 ms against the PIT and now stores it in
  `g_timer_count`. The PIT is masked afterward, and every LAPIC runs off the
  same bus clock, so APs need no recalibration: `lapic_timer_ap_arm()` just
  writes DCR=div16, LVT=periodic vector 48, ICR=`g_timer_count` on the calling
  CPU's own LAPIC.
- **D2 — arm in `smp_ap_entry`, before joining.** The AP arms its timer right
  after `lapic_ap_enable`, before `sched_ap_enter`. A tick during the pre-
  `sched_init` park phase is harmless (`sched_tick` returns on NULL `current`)
  and, as a bonus, wakes the AP from its park `hlt` to re-check `g_sched_ready`
  — so APs join promptly without needing an external kick. Once in the scheduler
  (phase 2), the tick decrements the running thread's quantum and `schedule()`s
  at expiry, exactly as on the BSP.
- **D3 — per-CPU tick counter for the proof.** `struct percpu` gains a `ticks`
  counter; `sched_tick` increments `this_cpu()->ticks`. Under cap-2b an AP's
  counter stays 0 (no AP timer); under cap-3 it advances — a clean, determinis-
  tic contrast. A ticking AP timer IS preemption capability (the tick calls
  `schedule()` on quantum expiry).
- **D4 — global tick side-effects stay BSP-only.** `timer_tick()` advances the
  GLOBAL time base — `g_ticks`, the vDSO wall clock, lwIP timers — which must
  tick once per REAL 10 ms, not once per CPU. So `isr_dispatch` runs the full
  `timer_tick()` only on the BSP; an AP's vector-48 tick calls ONLY `sched_tick`
  (its own preemption). Otherwise 4 CPUs would inflate `g_ticks` 4× and the
  wall clock with it — which shrank every `g_ticks`-based deadline 4× and flaked
  `smoke-crosswake` (the first symptom that caught this). Ring-3 signal delivery
  in `timer_tick` is likewise BSP-only for now — correct while user threads are
  BSP-pinned (cap-4 revisits it when APs run ring 3).

## Gate
`smoke-smppreempt` (`-smp 4`): the BSP records a non-BSP CPU's `percpu.ticks`,
waits ~300 ms, and asserts it advanced → `[smp] ap preempt OK` (`ap preempt
FAIL` forbidden). 60 gates.

## Non-goals (→ cap-4)
User threads on APs + per-CPU SYSCALL entry state; tickless idle; per-CPU
timer-frequency skew handling; priority/deadline scheduling.
