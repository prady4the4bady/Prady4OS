# DDR-756 — `SYS_SETNAME`: a process names itself (visible in `ps`)

**Status:** proposed (pre-code)
**Layer:** proc + syscall + user. On-theme for the named-agent OS.

## Problem

A process's name in `ps` (DDR-743/754) is fixed by the loader — the ELF filename
(`PRISM.ELF`, `KILL.ELF`, …). A process (or an AETHER agent) cannot label itself
with something meaningful ("kryos-worker", "compositor"). POSIX has
`prctl(PR_SET_NAME)`; PRADYOS has no equivalent, so the process table can't
reflect intent.

## Decision

Add a per-tcb name buffer and a self-only rename syscall (no kernel-structural
change beyond one field):

- `struct tcb` gains `char name_buf[16]`; `sched_create_state` sets
  `name_buf[0] = 0` (the "TCB fields are not zeroed" rule — kmalloc doesn't
  zero). `t->name` still points at the loader string until a rename.
- **`SYS_SETNAME` (NSI 75)** — `(const char *name) -> 0 | -EFAULT`. No capability:
  a thread may only rename *itself* (`current_thread`), never another — so there
  is no authority to escalate. `copyinstr` up to 15 chars into `t->name_buf`,
  NUL-terminate, then `t->name = t->name_buf`. `sched_snapshot` already reads
  `t->name`, so `ps` reflects the new name immediately.
- **PRISM `setname <name>`** — thin wrapper; then `ps` shows the shell renamed.

## Gate — `smoke-setname` (new; 91 → 92)

Self-contained: a freestanding probe calls `SYS_SETNAME("KILROY")`, then walks
`SYS_GETPROCS` to find its own pid (`SYS_GETPID`) and verifies that entry's
`name` is now `KILROY`. Round-trips the rename through the real process table.
Prints `PRADYOS_SETNAME_OK`, else `SETNAME FAIL`. Gate asserts the OK sentinel
(EXTRA_SENTINEL), `SETNAME FAIL` forbidden.

## Non-goals

- Self-rename only — no renaming other pids (that would need a capability model
  for the process table; out of scope).
- 15-char cap (matches the tcb `name[16]` / `procinfo.name[16]` width); longer
  names are truncated, not an error.
- No `SYS_GETNAME` (read is already covered by `SYS_GETPROCS`).
