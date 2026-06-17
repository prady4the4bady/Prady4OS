# Phase 2c — Process & Scheduling — Test Results

## Slice 1: TCB + asm context switch + preemptive round-robin

- **Date:** 2026-06-18
- **Decision:** ADR-008 (round-robin first; 3-lane NAS deferred).
- **Files:** `arch/x86_64/context.asm` (context_switch), `kernel/sched.{c,h}`
  (TCB, ready ring, sched_init/create/tick, yield, trampoline), `kernel/idt.c`
  (timer IRQ -> sched_tick after EOI), `kernel/console.c` (kputdec), `kernel/main.c`
  (TSC calibration, context-switch benchmark, two worker threads).

### Commands

```bash
make image && make smoke   # smoke PASS (NEXUS KERNEL OK)
```

### Verified (QEMU, 2.56 GHz AMD host)

```
NEXUS: TSC ~2563 MHz
NEXUS: context_switch ~275 cycles (~107 ns)  [target <= 1500 ns]
[thread B] iter=0 gtick=31
[thread A] iter=0 gtick=33
[thread B] iter=1 gtick=56
[thread A] iter=1 gtick=64
[thread B] iter=2 gtick=91
[thread A] iter=2 gtick=100
...
[thread B] done
[thread A] done
```

Interpretation:
- **Preemption is real:** the workers never call yield; the advancing `gtick`
  (PIT tick count) between A and B lines proves the timer IRQ drove the switches.
- **Context switch ~107 ns**, ~14x under the Layer-2 board's ≤ 1.5 µs target.
  TSC frequency was calibrated against the PIT (20 ticks = 200 ms).
- All earlier self-tests still pass; smoke PASS; `-Werror` clean.

### How switching works

Every switch (preemptive via the PIT IRQ, or cooperative via `yield`) goes
through the same `context_switch(&prev->rsp, next->rsp)`, which saves callee-saved
regs + RFLAGS. New threads are seeded with a matching frame whose RET enters a
trampoline. The PIT handler sends EOI *before* `sched_tick`, so the switch never
strands an un-acknowledged interrupt.

### Not done yet

- Priorities/lanes (the 3-lane NAS), sleep/block/wakeup, thread teardown + stack
  reclaim (finished threads remain in the ring and just halt), SMP.
- Next in Phase 2: capability system (NCS), IPC (NIA), syscalls (NSI).
