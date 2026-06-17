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

### Not done yet (slice 1)

- Priorities/lanes (the 3-lane NAS), sleep/block/wakeup, thread teardown + stack
  reclaim (finished threads remain in the ring and just halt), SMP.

## Slice 2: capability system (NCS)

- **Date:** 2026-06-18
- **Decision:** ADR-009 — opaque table-indexed handles (not the blueprint's
  weak 16-bit-MAC token).
- **Files:** `kernel/cap.{c,h}`; `tcb->caps` per-thread table (sched_init/create).

### Verified (QEMU)

```
NEXUS: capability system (NCS) tests
  validate R ok
  reject NET (not granted) ok
  guarded read allowed ok
  restricted: W denied ok
  restricted: R kept ok
  delegate cannot amplify W ok
  delegate keeps R ok
  revoked handle invalid ok
  guarded read now denied ok
  delegated cap survives revoke ok
  restricted cap survives revoke ok
NEXUS: NCS 11/11 passed
```

Confirms: rights enforcement, subset-only `cap_restrict`, non-amplifying
`cap_delegate`, O(1) `cap_revoke` via generation counters (and that independently
delegated/restricted caps survive the original's revoke), and the
guard-before-operation pattern (`demo_file_read` requires `CAP_FILE_R`).
smoke PASS; `-Werror` clean.

## Slice 3: synchronous IPC (NIA) + scheduler block/wakeup

- **Date:** 2026-06-18
- **Decision:** ADR-010 (sync first; async rings + broadcast bus follow).
- **Files:** `kernel/ipc/ipc.{c,h}`, `kernel/proc/sched.c` (block/wakeup + runnable
  skipping), `kernel/cap.c` (`cap_authorize` resource-bound check). Also: the
  `kernel/` tree was reorganized into `mm/`, `proc/`, `ipc/` subdirs (separate
  refactor commit) — see the repo layout.

### Verified (QEMU)

```
[recv] blocking on endpoint (no message yet)
[send] delivering message
[send] delivered
[recv] received: 0xFEEDFACECAFEBEEF 0x0102030405060708
[recv] send with recv-only cap correctly DENIED
```

Confirms: a receiver blocks (off-CPU) until a sender delivers; the sender wakes
it; the message arrives intact; and a recv-only capability is refused for send
(rights + resource binding via `cap_authorize`). smoke PASS; `-Werror` clean.

### Not done yet

- Async lock-free SPSC ring buffers; sovereign broadcast bus (next IPC slices).
- Sender does not block on a full slot (single outstanding message); larger /
  zero-copy payloads come with the async work.
- Syscalls (NSI), 3-lane NAS, APIC still ahead.
