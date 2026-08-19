# DDR-955 — `sched_block_timeout`: make every kernel wait bounded

Status: DESIGN ACCEPTED. Implementation NOT started (this DDR precedes the code,
per R16 / CLAUDE.md §3 "ADR/DDR before the code it governs").

## a. The defect class

`while (!done) sched_block_on(lk);` is an **unbounded** wait: if the wakeup is
ever lost or the producer dies, the thread blocks forever and its resources are
never reclaimed. Every such site is an S2 violation.

Live sites (`grep -rn "sched_block_on" kernel/`), all currently unbounded:

| site | waits for | proposed deadline |
|---|---|---|
| `virtio_blk.c:222` | a free request slot | 500 ticks (5 s) |
| `virtio_blk.c:260` | request completion | 500 ticks (5 s) |
| `ipc/ipc.c:60` | IPC endpoint message | 100 ticks (1 s) |
| `ipc/bcast.c:70` | broadcast subscriber | 100 ticks (1 s) |

`sched_block_on` itself is `sched.c:1304`.

## b. Answers to the §5 architecture checklist

1. **New NSI/syscalls:** none. Verified live max = **94**; nothing is consumed.
2. **TCB extension:** one field, `uint64_t block_deadline` (0 = no deadline).
   **`kmalloc` does not zero** (CLAUDE.md §0.6), so it MUST be explicitly
   initialised to 0 in `sched_create` alongside every other TCB field — this is
   the exact failure mode recorded in memory node `tcb-fields-not-zeroed`.
3. **PMM/VMM:** unaffected. No shared mapping, no new PTE class.
4. **Capability:** none. This is a kernel-internal primitive with no ring-3
   surface.
5. **AETHER queue/audit:** no new record type.
6. **Scheduler hooks:** `sched_create` (init the field), `timer_tick` (expiry
   scan), `sched_unblock` (reuse as-is).
7. **Filesystem:** none directly; `virtio_blk` is the first consumer, so every
   FS gate is the regression surface.
8. **Network policy:** none.
9. **Compositor/UI:** none.
10. **Gate determinism:** see §d — the gate must not depend on TCG timing
    (DDR-735/771 lesson).
11. **Security invariants:** S2 (bounded waits) is the one this *fixes*. No
    other invariant is touched; a timeout cannot grant authority.

## c. Two design corrections to the prescribed plan

**1. The expiry scan's lock is wrong as specified.** The instruction says the
timer-tick scan "runs under the per-CPU rq lock, not `g_sched_lock`, so no
inversion". But a blocked thread is **not on a runqueue** — that is what blocked
means. There is no per-CPU rq blocked list to walk under the rq lock. The scan
must therefore walk the thread table under `g_sched_lock`, which is the correct
inner lock and is what `sched_unblock` already takes. Walking a list that does
not exist would either not compile or silently scan nothing — a gate that then
"passes" would be measuring nothing (the R3/S18 class).

**2. The return-value contract is racy as specified.** "On wake: if
`g_ticks >= block_deadline` before done is set, return `-ETIMEDOUT`" makes the
verdict a *re-read* of two globals after waking — the same defect DDR-952 fixed
in `timer_tick`, where consumers re-read `g_ticks` instead of using the value
the tick produced. A completion that lands in the same tick as the deadline
would be reported as a timeout and the buffer discarded, corrupting a request
that actually succeeded. Instead the waker must record **why** the thread woke:
the expiry path sets a `blocked_timed_out` flag on the TCB before calling
`sched_unblock`, and `sched_block_timeout` returns based on that flag, not on a
clock comparison. One writer, one reader, no re-read.

## d. Gate design — `smoke-blk-timeout`

Three arms (R8, all discriminating):

- **A (timeout fires):** a test hook suppresses `req->done`. Assert
  `-ETIMEDOUT` is returned and `-EIO` propagates. Assert the kernel is still
  alive afterwards (a bounded wait that panics is not a fix).
- **B (normal path unaffected):** an ordinary read completes and returns 0.
  This is the arm that catches a deadline that is too aggressive.
- **C (no false timeout under load):** run the arm-B read while the block layer
  is contended; assert zero `[vblk] timeout` lines.

Determinism: arms assert on **sentinels and tick counts read from the guest**,
never on host wall-clock, because TCG timing is not reproducible.

Acceptance: `gate_rate.sh smoke-blk-timeout 20` = 20/20, plus
`fs_regression.sh` 9/9 unchanged — `virtio_blk` is under every FS gate, so a
regression there would surface as filesystem failures, not block-layer ones.

## e. Relationship to BUG-A (the SMP stall)

If the stall is a thread blocked forever on a lost `virtio-blk` wakeup, this
primitive converts it from a silent hang into a loud, attributable `-EIO` with a
named device and LBA. That is **diagnostic value, not proof of a fix** — and it
must not be reported as closing BUG-A. BUG-A is closed only by the DDR-777
discriminator naming the mechanism, per CLAUDE.md §0.2.
