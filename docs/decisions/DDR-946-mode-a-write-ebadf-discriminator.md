= DDR-946 — mode A: discriminate "main not entered" from "the write failed"

**Status:** ACCEPTED (instrument). **No fix.**
**Date:** 2026-08-16
**Lineage:** DDR-936 → DDR-940 → DDR-945 (framing refuted) → **DDR-946 (this)**.
**Numbering:** DDR-945 is taken (mode-A refutation, `97b9ca2`). Verified 946
free in both `docs/ddr/` and `docs/decisions/` per §0.4.

## What is already known, and what it forecloses

DDR-945 measured, on a failing run:

```
PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82
sys_exit(0) pid=82
```

**`sys_exit` is a syscall issued from ring 3.** A thread that was never
unblocked, never scheduled, or never entered ring 3 cannot issue one. So the
following are already excluded *by this line alone*:

- the thread was never unblocked;
- the thread never reached ring 3;
- the ELF entry point was never executed.

**This is why the planned `[agent-exec]` marker at `sched_unblock` would not
help.** It would test "was this thread unblocked / did it start", a question
`sys_exit(0) pid=82` has already answered *yes*. A marker whose both outcomes
are already known is not a discriminator. Recording that explicitly because the
task plan specifies it, and building it would have consumed a CI cycle to
confirm something the existing log already shows.

## The two candidates that remain

1. **`main` was never entered.** musl's `_start` / `__libc_start_main` returned
   or exited 0 before calling `main`. Ring 3 ran (hence the syscall), but the
   agent's first `printf` was never reached.
2. **`main` was entered and the write failed.** `printf` →
   `SYS_WRITE(1, …)` → `sys_write` does
   `fd_get(current_thread, 1)`; if fd 1 is unwired it returns `-EBADF`, musl
   discards the error, and `main` proceeds to a normal `exit(0)` — invisibly.

### A gap in my own DDR-945 reasoning

DDR-945 refuted "stdout not wired" using a **passing** run: four triggers → four
`AGENT_START`/`DONE` pairs → those four pids exiting. That proves stdio was
wired **on that run**. It does **not** prove stdio was wired on the *failing*
run. If the wiring is itself intermittent, candidate 2 survives, and DDR-945's
refutation was scoped more broadly than its evidence supports. Correcting that
here rather than leaving it to be inherited.

## The instrument

Log the **first** `-EBADF` write per thread, from the kernel:

```
[fd] write EBADF pid=<pid> fd=<n>
```

Kernel-side (`kputs`), so it cannot be swallowed by the very I/O path under
suspicion. First-occurrence-only per thread, so a process looping on a bad fd
cannot flood the log — the same volume discipline as DDR-941's on-change
`BTN_STATE`.

## Reading it

- **`[fd] write EBADF pid=<agent pid> fd=1` present on a failing run** ⇒
  candidate 2. `main` ran, the write was rejected, the agent worked invisibly.
  The defect is in fd wiring on the spawn path, and it is intermittent.
- **absent on a failing run** ⇒ candidate 1. `main` was never entered; the
  defect is before `main`, in musl crt / the ELF entry setup, and the next
  instrument is a kernel-side marker at the *first ring-3 instruction*, not at
  unblock.

Both outcomes name a different subsystem, which is what the `[agent-exec]`
marker could not do.

## What would refute this instrument's usefulness

If a failing run shows **no** `EBADF` **and** the agent still exits 0, then
`main` was not entered and this instrument has done its job by exclusion — that
is a valid result, not a failure. The instrument is only useless if it never
fires on either a passing or failing run *and* the agent's behaviour is
unchanged, which would mean writes are not going through `sys_write` at all.

## Not doing

No fix. Not wiring fds, not touching the spawn hook, not changing the gate
assertion. Ten mechanisms have been retired in this investigation, every one by
an instrument and none by argument; the eleventh will be no different.

---

## Readout — and mode A splits AGAIN (local, tip `2a20001` + this instrument)

`smoke-agent-click` x12 local, **2 failures**, both the *second* assertion
("the clicked PRAX agent did not run to completion") — i.e. both mode A.
`btnedge=5` on both, so input reached the driver and this is not mode B.

### The discriminator reads NEGATIVE

| observation | run 1 | run 3 |
|---|---|---|
| `[fd] write EBADF` for a **triggered agent pid** | none | none |
| `PRADYOS_AGENT_START` anywhere in the log | **0** | **0** |
| triggered pids | (interleaved) | 50, 55 |
| `sys_exit` for those pids | — | **0 — never exited** |

The only `EBADF` lines are `fd=99` and `fd=3735928559` (= `0xDEADBEEF`), both
poison values from deliberate negative-test probes, not the agent.

**So `sys_write` was never called by the agent at all**: a rejected write would
log `EBADF`, a successful one would print `AGENT_START`. Neither happened.
Candidate 2 ("main entered, write failed") is **refuted** for these failures.

### But the bigger result: these are NOT the same failure as DDR-945's

DDR-945's CI failure had `sys_exit(0) pid=82` — the agent **ran and exited**.
These local failures have the triggered pids **never exiting at all**.

**Mode A is not one defect.** It has at least two sub-signatures:

- **A1** (DDR-945, CI 31958185299): agent runs, exits 0, prints nothing.
- **A2** (here, local): agent never exits, never prints — consistent with a
  thread that never ran, which is DDR-936's original framing.

DDR-945 said "DDR-936's framing is refuted for mode A". That is **too broad**:
it is refuted for **A1**, on the single run it was measured from. For **A2**
DDR-936's framing may still hold. Correcting the scope here rather than letting
the over-generalisation stand — §6.0-C applies *within* mode A, not just
between modes.

### What this means for the instruments already shipped

A2 is the signature the `ubcas`/`ubrq`/`rqmiss`/`rqdepth` counters were built
for. They must be read **on an A2 failure specifically** — the previous readings
came from runs that may have been A1, which is why they showed nothing.

### Next

1. Re-read the strand counters on a confirmed **A2** failure (triggered pid
   present, no `sys_exit` for it). Prior zero readings do not transfer.
2. Classify every future `smoke-agent-click` failure as A1 or A2 **before**
   using its data — the discriminator is whether the triggered pid exits.
3. Do not merge A1 and A2 evidence. That is the mistake this whole
   investigation keeps rediscovering, now at a third level of granularity
   (mode A/B, then A1/A2).
