# DDR-714 stage C3 — distribute virtio-blk vectors across APs

> DDR before code. The finale of stage C: device interrupts leave the BSP.
> Scope is deliberately **blk only** — the locks-2 D2 completion review done
> here shows net and input must stay BSP-routed for now (D4).

## The completion review (locks-2 D2, due now)
With a blk vector routed to an AP, the completion handler (`complete(v)`: pop
used ring, `done=1`, wake waiter) runs on CPU B while the requester sleeps on
CPU A. Three hazards:

1. **Lost wakeup** (the locks-4 class): `submit()` waits with `cli; while
   (!done) sched_block();` — `cli` masks only CPU A, so CPU B's IRQ can fire
   between the `done` check and the BLOCKED transition; its `sched_unblock`
   CAS on a not-yet-BLOCKED thread is a no-op → the requester sleeps forever.
   Fix: the locks-4 pattern — a per-device `spinlock_t compl_lock` guarding
   `done`/`waiter`; the IRQ handler takes it (irqsave), the requester waits via
   `sched_block_on(&compl_lock)`, which publishes BLOCKED under the lock.
2. **virtq cross-CPU ordering**: submit (CPU A: `virtq_add`+`publish`) and
   completion (CPU B: `pop_used`+`free_chain`) never overlap in time for one
   device (the `busy` sleep-mutex serializes requests, and the device only
   raises the vector after the publish), but the MEMORY ordering must be
   explicit: `virtq_publish` already ends in `virtio_mb()`, and the used-ring
   read on CPU B is ordered by the interrupt delivery itself + the acquire on
   `compl_lock`. No additional virtq lock needed while one-in-flight holds.
3. **The `cli` in submit** becomes the spinlock's irqsave (same local masking,
   plus the cross-CPU exclusion).

## Decisions
- **D1 — blk vectors round-robin over the APs.** blk unit i → CPU roster idx
  `1 + (i % (ncpus-1))` when APs exist (falls back to the BSP single-CPU).
  Uses `lapic_apic_id_at()`; the wake IPI proves APs take interrupts already.
- **D2 — per-device completion lock + sched_block_on** (hazard 1/3 fix above).
- **D3 — the proof.** A per-device `compl_cpu` records `this_cpu()->cpu_idx`
  in the handler; after the FS phase the BSP prints `[blk] msix on AP OK` if
  any disk completed on a non-BSP CPU (poll flag, print outside handlers).
  Gate `smoke-msixap` (`-smp 4`) asserts it (+FS sentinels still green).
- **D4 — net + input STAY on the BSP** (documented non-goal): net's completion
  calls into lwIP (`g_rx_cb`) whose timers/state run BSP-only and are not
  thread-safe — distributing it means locking the whole lwIP layer; input folds
  into globals read by BSP pollers. Both are low-rate; no benefit.

## Gate
`smoke-msixap` (`-smp 4`): `[blk] msix on AP OK` + the FS sentinels. 62 gates.

## Non-goals
net/input distribution (D4); per-queue vectors; multi-in-flight requests;
IRQ balancing/affinity API; the ISA lines / I/O APIC (a later decision — the
8259 keeps keyboard+COM1 fine).
