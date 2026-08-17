= DDR-948 — A1: count the agent's write ATTEMPTS and report them at exit

**Status:** ACCEPTED (instrument). **No fix.** Also records the Phase-A1 TCB audit.
**Date:** 2026-08-16
**Lineage:** DDR-945 (A1 measured) → DDR-946 (EBADF discriminator) → **948**.
**Numbering:** 945/946/947 taken; 948 verified free in **both** `docs/ddr/` and
`docs/decisions/` (§0.4). *The directive's "next free = DDR-947" is stale — 947
was allocated last session for the A2 runqueue reading.*

## Part 1 — Phase A1: full TCB initialiser audit (§0.6)

The directive asks to confirm the DDR-946 field is initialised. I audited **all
64 `struct tcb` fields** against `sched_create_state`, not just the new one,
because §0.6's failure mode is silent and SMP-intermittent.

**Result: 62 of 64 explicitly initialised. Two are not:**

| field | initialised? | guarded by | live defect? |
|---|---|---|---|
| `fork_regs` | no | `t->forked`, initialised `= 0` (`sched.c:904`); read only under `if (t->forked)` (`sched.c:963`) | **no** |
| `sig_saved` | no | `t->sig_active` (initialised); `sys_sigreturn` returns `-EINVAL` unless set, and `signal_deliver` writes `sig_saved` before setting it | **no** |

`dbg_ebadf_seen` (DDR-946) is initialised at `sched.c:839`. **§0.6 is satisfied;
no fix required.** Both unguarded fields are `struct regs` read only behind an
initialised flag. Recorded rather than "fixed" because adding redundant
initialisers to two large structs on every thread creation is a cost with no
correctness gain, and the audit's value is the *record* that they are guarded.

## Part 2 — the A1 instrument

### Why the specified marker will not work

The plan (F5) says: add an exec-entry marker "immediately before the first
`sys_write` in the agent's `main()`". If that marker is a ring-3 `printf`, it
goes through **the same `sys_write` path under suspicion** — if writes are being
lost, the marker is lost too, and its absence would be uninformative. A
discriminator whose failure mode is identical to the thing it discriminates is
not a discriminator.

DDR-946 already established the surrounding facts: on an A1 failure there is
**no `EBADF`** for the agent pid and **no `AGENT_START`**, so `sys_write` was
either never called or called and lost. `sys_exit(0)` proves ring 3 ran.

### The instrument

Count `sys_write` **attempts** per thread in the TCB, and report the count from
the kernel at `sys_exit`:

```
[user] sys_exit(0) pid=82 writes=<n> — thread terminating
```

Kernel-side, so it cannot be swallowed. `writes` counts entries to `sys_write`
**before** any fd validation, so it separates "never called" from "called and
rejected/lost".

### Reading it on an A1 failure (pid IS in a `sys_exit` line)

- **`writes=0`** ⇒ the agent never attempted a write ⇒ `main` was never entered
  (musl crt exited 0), or it exited before the first `printf`. The defect is
  **pre-`main`**, in the ELF entry / crt / auxv setup.
- **`writes>0`** with no `AGENT_START` and no `EBADF` ⇒ writes were attempted
  and **accepted** yet produced no serial output ⇒ the defect is downstream of
  `fd_write_user` — the console/UART sink for that thread's fd.

Those name two different subsystems, which is what this instrument is for.

## What would refute this instrument's value

If `writes=0` on A1 **and** `writes=0` on a *passing* run's clicked agent, the
counter is not being incremented on the path the agent uses (e.g. it writes via
`writev`, not `write`). **`sys_writev` must be counted too** — musl stdio issues
`writev` with 2 iovecs (noted at `sys_io.c:150-152`), so counting only
`sys_write` would read 0 on every run and prove nothing. Both are counted.

That caveat is the whole reason this DDR exists rather than a one-line patch:
the obvious version of this instrument would have silently measured nothing.

## Not doing

No fix. No change to the spawn hook, the fd table, the crt, or the gate
assertion. Twelve mechanisms retired so far in this investigation, every one by
instrument and none by argument.
