# DDR-743 — SYS_GETPROCS: ring-3 process listing (`ps` works)

**Status:** proposed (pre-code)
**Layer:** syscall + proc + user (PRISM). Sibling of DDR-742; the other shell stub.

## Problem

PRISM's `ps` only prints its own pid (`"minimal; full ps pending a
process-table syscall"`). Ring 3 cannot enumerate processes: the scheduler ring
is kernel-internal (a circular `tcb.next` list guarded by `g_sched_lock`), with
no read-only introspection surface. With `ls` now working (DDR-742), `ps` is the
last shell stub.

## Decision

**`SYS_GETPROCS` (NSI 67)** — `(index, out_buf) -> 1(filled) | 0(end) | -errno`,
per-entry to match the `getdents` style. `out_buf` receives a fixed
`struct procinfo { uint32_t pid, ppid, state, flags; char name[16]; }` (56 bytes,
no pointers — pure value copy). `flags` bit 0 = is_user. `state` is the raw
`THREAD_*` enum (0 ready, 1 running, 2 done, 3 blocked, 4 zombie).

**Kernel side (a proc introspection helper).** The walk must run under
`g_sched_lock` (the ring is mutated by create/exit/reap on any CPU), and
`sys_proc.c` must not touch the ring directly. So `sched.c` exposes
`sched_snapshot(int index, struct procinfo *out) -> 1|0`: it takes the lock,
walks `current_thread->next…` counting to `index`, copies that tcb's
`pid/parent_pid/state/is_user` and a bounded copy of its `name` into `out`, and
returns. `SYS_GETPROCS` calls it and `copyout`s (never under the lock — snapshot
first, copy out after). A racing create/exit between indices only adds/drops a
row, which `ps` tolerates (a best-effort listing, like real `ps`).

**PRISM `ps`** loops `SYS_GETPROCS(i, &pi)` from `i = 0` until it returns `<= 0`,
printing a header then `pid ppid S name` per row (`S` = a one-letter state:
R/r/D/B/Z). It shows all threads (kernel + user) so the operator sees init,
prism, the daemon, the idle/reaper kernel threads, etc.

## Gate — extend `smoke-shell` (no new gate; stays 83)

`smoke-shell` already drives PRISM. Add a `ps` command before `exit` and assert
the serial shows a `PID` header line (printed only by the new `ps`) — proving
the syscall end-to-end. The existing echo/help/ls/no-panic assertions are
unchanged. (Asserting a specific pid is timing-fragile; the header + non-empty
listing is the deterministic witness.)

Regression: `smoke-shell`, `smoke-syswait`/`smoke-sysfork` (the ring is walked
while procs come and go), the SMP set (ring mutated cross-CPU), then the full
suite.

## Incidental fix — DDR-742 `ls` gate flake

The `ls` assertion `^HELLO\.TXT$$` was flaky: PRISM flushes `prism> ` before
`readline`, so depending on flush/read timing the first `ls` output line is
either bare `HELLO.TXT` or `prism> HELLO.TXT`. CI hit the latter and went red
(run 29260157525). Relaxed to `(^|prism> )HELLO\.TXT$$` — still trailing-anchored,
so it excludes the kernel's `    HELLO.TXT  25 bytes` and `[fs] /HELLO.TXT:`
lines. The `ps` header assert avoids the same trap by not anchoring to BOL.

## Non-goals

- No CPU%/memory columns (run_ticks/dispatches exist per DDR-735 but are agent-
  roster-scoped; a full top-style view is a later slice); name only + state.
- No kill-by-name or job control; `ps` is read-only.
- No `/proc` filesystem — a single introspection syscall, not a synthetic FS.
