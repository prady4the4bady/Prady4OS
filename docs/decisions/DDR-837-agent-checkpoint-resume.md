# DDR-837 — agent checkpoint / resume: NSI 84/85

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/proc/sched.{c,h}`, `kernel/syscall/sys_checkpoint.c`
**Section:** E (NSI 84/85)

## What this is

The operator freezes a running agent and thaws it later:

```
SYS_CHECKPOINT_AGENT  84   (pid)   CAP_SOVEREIGN
SYS_RESUME_AGENT      85   (pid)   CAP_SOVEREIGN
```

## The agent blocks ITSELF, at a syscall boundary

The obvious implementation — have the checkpointing thread set the target's state
to `THREAD_BLOCKED` directly — is **wrong and unsafe**. The target may be
`THREAD_RUNNING` on another CPU at that instant, mid-way through arbitrary kernel
work, holding locks. Marking it blocked from a second CPU races the scheduler for
its state field and can strand whatever it holds.

Instead `SYS_CHECKPOINT_AGENT` sets a flag, `tcb.checkpointed`, and the target
observes that flag **itself** in `syscall_dispatch`, immediately before the
handler runs, and calls `sched_block()`. That point is already a safe boundary:
it is where the existing AETHER rate limiter kills an over-budget agent, so the
kernel is known to be in a state where a thread may cleanly stop there.

Consequence, stated so it is not a surprise: **an agent is frozen at its next
syscall, not instantly.** An agent in a long pure-computation loop keeps running
until it calls something. That is the honest cost of not racing the scheduler.

## Guards

- **pid 1 (init) cannot be checkpointed** — freezing init wedges the system, and a
  syscall that can wedge the machine on a valid-looking argument is a footgun
  regardless of who may call it.
- **A thread cannot checkpoint itself.** It would block inside the syscall that
  requested the block, and only another sovereign could free it — trivially
  reachable by a typo.
- Unknown pid returns `-ESRCH`, distinct from `-EPERM`: "no such agent" and "not
  allowed" are different operator facts.

## Why CAP_SOVEREIGN on both

Freezing an agent is a denial of service against it; resuming one restores an
agent the operator may have frozen deliberately. Neither belongs to CAP_AGENT —
an agent able to resume itself makes the freeze advisory, and one able to freeze
its peers can silence the very agents that would report on it.

## Audit

`AR_AGENT_CHECKPOINT` / `AR_AGENT_RESUME`, **appended** per DDR-832. These are
recorded because "why did this agent stop producing?" is otherwise unanswerable
from the log, and the answer "an operator froze it" is exactly the kind of fact
that looks like a fault when it is missing.

## The gate — `smoke-checkpoint`, four arms

Observation is via `SYS_GETPROCS`, whose `procinfo.state` carries the raw
`THREAD_*` enum. **The gate asserts on observed state, not on elapsed time** — a
timing-based gate for a scheduling feature is a flake generator.

1. checkpoint a live agent → its state becomes `THREAD_BLOCKED`
2. resume it → its state leaves `THREAD_BLOCKED`
3. checkpoint an unknown pid → `-ESRCH`
4. checkpoint from a non-sovereign caller → `-EPERM`, audited

The target agent alternates a pure-computation delay with a single `SYS_YIELD`,
deliberately: agents are capped at 60 syscalls/s (ADR-026 D7) and a spin on
syscalls would be killed at 137 before the gate could observe anything.

## The rule this earns

**A "stop that thread" operation must be executed by the thread being stopped.**
Any other implementation is a cross-CPU write to state the scheduler owns, and it
is wrong on exactly the runs where it matters — the ones where the target is
actually running.
