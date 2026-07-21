# ADR-032 — FS write budget: token-bucket rate limit (supersedes ADR-015's lifetime cap)

**Status:** accepted. Supersedes the per-thread FS write-budget mechanism of
**ADR-015** (that ADR's other elements — CAP_FS_READ/WRITE, the mount model —
stand unchanged). Per the project rule that a binding ADR is superseded by a new
ADR, this is that ADR.

## Context

ADR-015 gave each thread a **lifetime** FS write budget (`tcb.fs_write_budget`,
`FS_WRITE_BUDGET_DEFAULT = 1 MiB`): `vfs_write` refuses once the thread has
written 1 MiB total, ever. This bounds a hostile/buggy consumer's total device
writes — but it also **permanently** stops a legitimate long-running process
after 1 MiB, which is far too little for any real workload (a shell session, a
log writer, the AETHER daemon). The cap conflates two distinct concerns:

- **write RATE** (anti-flood: don't let one thread saturate the device), and
- **disk SPACE** (don't let one thread fill the volume).

## Decision

Replace the lifetime cap with a **token-bucket rate limiter** on `tcb`:

- `fs_write_budget` becomes the current token balance (bytes); add
  `fs_budget_tick` = the `g_ticks` value at the last refill.
- Bucket cap `FS_WRITE_BURST_MAX = 1 MiB` (max single burst / idle accumulation).
- Refill `FS_WRITE_REFILL_PER_TICK = 256 KiB` per 100 Hz tick → **25 MiB/s**
  sustained ceiling per thread.
- `vfs_write` lazily refills before the check: `add = (now - last_tick) *
  REFILL`, top up to at most `BURST_MAX`, then require `balance >= len` and
  decrement by bytes written.
- **Refill only tops up a balance BELOW the cap; it never reduces a higher
  balance.** This preserves the kernel self-test bypass (`fs_write_budget =
  ~0ull`), which several boot self-tests use to exercise the FS engine rather
  than the budget.

`sched_create` initialises `fs_write_budget = FS_WRITE_BURST_MAX`,
`fs_budget_tick = g_ticks`.

## Security rationale

- **Anti-flood is preserved and arguably improved**: a hostile thread is bounded
  to 25 MiB/s sustained (plus a 1 MiB burst), where before it could burst 1 MiB
  instantly then stop — the new model bounds the *rate* continuously.
- **Disk-space exhaustion is a separate control**, not this one: the SFS
  free-space GC (DDR-762) reclaims unlinked space, and per-mount space quotas are
  future work. The lifetime cap was a poor proxy for space control (it blocked
  legitimate use while a process could still fill the disk by unlink-churning).
- No new authority is granted: `CAP_FS_WRITE` is still required; this only changes
  how an already-authorised writer is throttled.

## Gate

A kernel self-test writes **more than `BURST_MAX`** (1.5 MiB) from one thread in
64 KiB chunks with `yield()` between refills, and asserts the total succeeded →
`PRADYOS_FS_BUDGET_OK`. Under the old lifetime cap this is impossible (caps at
1 MiB); under the token bucket it completes as tokens refill.
