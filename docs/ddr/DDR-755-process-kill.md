# DDR-755 — process signaling: kill end-to-end + PRISM `kill`

**Status:** proposed (pre-code)
**Layer:** user (probe + PRISM). Consumes the existing `SYS_KILL`; no kernel change.

## Problem

`SYS_KILL` (NSI 23, PROC-C) + `signal_deliver` are implemented — SIGKILL is
non-maskable and always terminates, SIGTERM with no handler terminates — but
there is no end-to-end proof that one ring-3 process can terminate another, and
no way to do it interactively. `ps` (DDR-754) shows processes; the matching
*control* verb, `kill`, is missing from the shell, and the kill→reap path has no
gate.

## Decision

No kernel change (the machinery exists). Two additions:

- **`killtest` probe** (freestanding, `user.ld`): `fork()`s a child that spins in
  ring 3 forever (`pause` loop — never exits on its own); the parent yields a few
  times so the child is scheduled, `SYS_KILL(child, SIGKILL)`, then
  `SYS_WAIT4(child)`. Reaching past `wait4` is itself the proof: the child has no
  exit path, so the *only* way `wait4` returns is if the signal terminated it.
  Prints `PRADYOS_KILL_OK`. Spawned like the other freestanding probes.
- **PRISM `kill <pid> [sig]`** — `SYS_KILL(pid, sig|SIGTERM)`; prints
  `kill: sent <sig> to <pid>` on success or `kill: pid <pid> not found` on
  `-ESRCH`. Added to `help` + dispatch.

## Gate — `smoke-kill` (new; 90 → 91)

`EXTRA_SENTINEL=PRADYOS_KILL_OK`, `FORBIDDEN_SENTINEL=KILL FAIL` via
`boot_test.sh`. Deterministic: the child's infinite loop makes `PRADYOS_KILL_OK`
appear *only* if the kill+reap path works (if it did not, `wait4` would block and
the sentinel would never print — a clean timeout FAIL, no false pass). On slow TCG
the timer-driven signal delivery may take a few quanta, but it always completes
within the boot-test window. PRISM `kill` is a thin wrapper over the same NSI, so
it needs no separate gate.

## Non-goals

- No new signals or handlers; SIGKILL/SIGTERM defaults only (SIGACTION already
  exists for catchable signals, PROC-C).
- No `kill -l`, no process-group / negative-pid semantics, no `killall`.
- No job control / backgrounding in PRISM (deferred, ADR-024) — `kill` takes an
  explicit pid (e.g. from `ps`).
