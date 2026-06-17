# ADR-008: Scheduler — round-robin first, 3-lane NAS deferred

- **Status:** Accepted 2026-06-18 (user-approved)
- **Phase:** 2c

## Context

The Layer-2 board specifies a 3-lane adaptive scheduler (NAS): Deterministic
(real-time), Throughput (weighted fair queue), Interactive (urgency-decay), plus
an AI-hint lane. That is a large, tunable system — and it cannot be tuned
meaningfully without real workloads (agents, I/O) that do not exist yet.

## Decision

Build the **minimum correct preemptive scheduler first**, then evolve it toward
the NAS:

- Minimal TCB (`struct tcb`): saved RSP, kernel stack base, tid, state, quantum.
- Hand-written **assembly context switch** (`arch/x86_64/context.asm`) saving the
  SysV callee-saved regs + RFLAGS; benchmarked at boot.
- A single **circular ready ring** (no lanes), round-robin, quantum = 2 PIT ticks
  (20 ms). The PIT IRQ (`sched_tick`) decrements the quantum and switches on
  expiry; `yield()` provides a cooperative switch.
- New threads are seeded with a context-switch frame whose RET enters a
  trampoline (RFLAGS = IF set), so they start interruptible.

The full 3-lane NAS and AI-hint lane are deferred until there are real workloads
to schedule and measure.

## Consequences

- Correct, measurable multitasking now (verified: two threads interleave under
  timer preemption; context switch ~275 cycles / ~107 ns at 2.56 GHz, well under
  the board's ≤ 1.5 µs target).
- Not yet present: priorities/lanes, sleep/block/wakeup, thread teardown +
  stack reclaim (finished threads stay in the ring and just halt), SMP. These
  come with the NAS and process work.
- The switch path goes through the IRQ for preemption and directly for `yield()`;
  both use the same `context_switch`, so all saved states are uniform.
