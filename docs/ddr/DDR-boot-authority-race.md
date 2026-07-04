# DDR — Process authority flags must be set before first run

> Root-cause DDR for the recurring `smoke-agents` CI failure (runs 28341424605,
> 28614622428, 28689681992 — GitHub-only, never local).

## The bug
`elf_load` creates **and enqueues** the new ring-3 thread; kmain's FS-phase
code then sets authority flags (`is_sovereign` on the compositor and AETHER
daemon, `is_agent` on spawned agents) **after** `user_boot_from_sfs` returns.
The scheduler is preemptive, so on a slow/loaded host the new thread can run
its first syscalls *before* the flag lands: the daemon's `SYS_SET_MODE`
self-check gets `-EPERM` (`PRADYOS_MODE_TOGGLE_FAIL`) and its
`SYS_SPAWN_AGENT` returns `-EPERM` (= `rc=-1`) → KRYOS never lights →
`smoke-agents` misses `AGENT KRYOS active`. Locally the race never loses;
under CI TCG load it sometimes does — the same class as the DDR-713 spawn-hook
race. The earlier `smoke-agents` timeout bump treated a symptom; this DDR
removes the race.

## The fix
`sched_create_user` now creates the thread **BLOCKED**; the loader's caller
grants authority and then `sched_unblock`s it (the DDR-SMP-3c-locks-1 CAS —
also correct cross-CPU):
- `user_boot_from_sfs(..., int sovereign)` sets `is_sovereign` before the
  unblock; all 13 call sites updated (compositor + daemon pass 1).
- `aether_spawn_agent_hook` sets `is_agent` + `parent_pid` before the unblock.
No process can ever observe its own pre-authority window again. `sys_execve`
is unaffected (it reuses the calling thread via `elf_build_image`); fork's
clone path already fully initializes the child before enqueueing it.

## Gate
No new gate: `smoke-agents`/`smoke-aether`/`smoke-mode` assert the fixed
behavior on every run; the race window itself is gone by construction.
