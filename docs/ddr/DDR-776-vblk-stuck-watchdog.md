# DDR-776 — virtio-blk stuck-request watchdog (diagnosis before fix)

**Status:** proposed (pre-code).
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 3**
(`-smp 4` hang). Parent: `docs/ddr/DDR-775-smp4-blk-hang.md` (evidence + confirmed
defect).

## Problem

DDR-775 narrowed the intermittent `-smp 4` boot hang to the virtio-blk completion
path and confirmed a defect: `submit()`'s completion wait is **unbounded**, so a
missed completion hangs forever and the gate can report only "sentinel not found".
The true trigger — missed IRQ vs. lost `virtq_pop_used` vs. `head2slot` corruption
— is still **unknown**, and the bug **does not reproduce locally** (3/3 pass) and
is **intermittent in CI** (the same block gates passed in run 30158060606 after
failing in 30155872016).

Those two facts together mean any behavioural fix right now is a guess validated
only by a ~2.5 h CI run whose signal is itself intermittent. **Diagnosis must come
first.**

## Design decision (the choice DDR-775 left open)

DDR-775 framed the next step as bounding the wait, with two options. Both are
rejected *for this slice*, and a third is chosen:

- **(a) Scheduler timed-block** — add a timeout variant of `sched_block_on()`.
  This is the **correct eventual primitive**: it bounds the wait while preserving
  the deliberate "sleep, never spin" property. **Rejected for now** because it
  modifies the scheduler core — used by every thread on every CPU — *while an
  intermittent SMP hang is unexplained*. Changing the scheduler mid-investigation
  would confound attribution of the very bug being chased.
- **(b) Bounded `g_ticks` yield-loop in the driver** — **rejected outright**: it
  would spin-yield on **every** block I/O, not just the rare failing one,
  regressing the hot path that every filesystem gate rides, and surrendering the
  "sleep, never spin" property permanently — a large, always-on cost to catch a
  rare event.
- **(c) CHOSEN — a passive stuck-request watchdog.** Change **no** blocking
  behaviour. Record each request's submit tick, and from the existing timer path
  print **once** per stuck request:
  `[vblk] stuck dev=D slot=N lba=L age=T`.

Why (c) works even though the submitter is hung: only the *waiting thread* is
blocked. The timer interrupt keeps firing and other threads keep running, so a
timer-driven print still reaches the serial log — well before the 180 s gate
timeout. The next CI failure therefore **names the stuck request** instead of
being silent, which is exactly what makes the trigger decidable.

This is idiomatic here: `net_poll_tick()` is already driven from the timer path in
`idt.c` (throttled with `g_ticks % 10`), so a throttled driver tick-hook is an
established pattern, not a new mechanism.

## Decision — implementation

1. `struct vreq` gains `uint64_t t0` (submit tick) and `int warned` (print-once).
   Both set when the slot is claimed in `submit()`.
2. `void virtio_blk_watchdog(void)` scans every registered instance's `VBLK_NREQ`
   slots; for a slot that is `used && !done` with `g_ticks - t0 > VBLK_STUCK_TICKS`
   (500 ticks = 5 s @100 Hz) and `!warned`, it prints the line above and sets
   `warned` so it fires **once per request** (no log spam — bounded, **S2**).
3. `idt.c` calls it from the timer path, throttled (`g_ticks % 100`, i.e. ~1 s),
   next to the existing `net_poll_tick()` call.
4. **Lock-free reads on purpose.** The watchdog runs in interrupt context and
   deliberately does **not** take `compl_lock`: acquiring a driver lock from the
   timer ISR adds a deadlock surface to the exact subsystem under investigation.
   The fields are `volatile` scalars and this is diagnostic output only — a torn
   read can at worst print a stale LBA, which cannot affect correctness (**S6**:
   the watchdog can never mutate driver state or fail an I/O).

## Gate

No new gate and no sentinel change. The existing block gates
(`smoke-fs`, `smoke-blkmq`, `smoke-blk-integrity`, `smoke-surfdestroy`) must stay
green, proving the watchdog is inert on the happy path. Gate count stays **106**.
`[vblk] stuck …` is deliberately **not** added as a FORBIDDEN sentinel yet: it
should be free to appear in a failing run so the log captures it. Once the trigger
is fixed, it becomes a forbidden pattern.

## What this slice does and does not claim

It **does not fix** the hang and must not be described as doing so. It converts a
silent 180 s timeout into a named, timestamped stuck request. Bounding the wait
(option (a), the real S2 fix) remains the follow-on, and will be designed with the
diagnostic's output in hand rather than speculatively.

## Architecture prerequisite checklist

- New NSI/syscalls: none. TCB/roster fields: none. PMM/VMM mappings: none.
  Capability gates: none. AETHER queue/audit: none.
- **Scheduler hooks: none** — this is precisely why (c) was chosen over (a); the
  timer *call site* in `idt.c` follows the existing `net_poll_tick()` pattern and
  adds no scheduler behaviour.
- Filesystem/root-mount: none directly (observes the path every FS gate rides).
- Network policy tables: none. Compositor/UI: none.
- New smoke gate: none; existing block gates prove non-regression.
- **Security invariants:** **S2 (bounded everything)** — the watchdog is bounded
  work per tick (≤`VBLK_MAX`×`VBLK_NREQ` = 64 scalar checks) and prints once per
  request, so it can neither spin nor spam. **S6 (fault isolation)** — read-only,
  takes no lock, mutates no driver state, and cannot fail an I/O or panic.
  S1/S3–S5/S7/S8 are not engaged. Nothing touches W^X, NX, or capabilities.
