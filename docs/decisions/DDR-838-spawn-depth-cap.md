# DDR-838 — spawn-depth cap: bounding agent self-replication

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/proc/sched.{c,h}`, `kernel/syscall/sys_fork.c`
**Section:** E

## The problem

An agent that forks, whose child forks, and so on, multiplies without bound. The
existing controls do not stop it: the syscall rate limiter (ADR-026 D7) bounds
one agent's calls per second, and `aether_mem` bounds one agent's memory — but N
agents each within their own budget is still N times the load, and N grows
geometrically.

## The trap: `is_agent` is NOT inherited across fork

`sched_create` zeroes every authority flag, and `fork` does not re-grant them:

```c
t->is_agent = 0;      /* L6: not an AETHER agent unless the spawner sets it */
t->is_sovereign = 0;
t->is_net = 0;
t->is_memory = 0;
```

That is correct for authority — a forked child should not silently inherit
capabilities. But it means **a cap conditioned on `is_agent` is trivially
escaped**: the agent forks once, the child is not an agent, and the child and
everything below it fork freely. The cap would appear to work in a one-level test
and do nothing in practice.

So the cap must follow **lineage**, which fork does propagate, rather than
capability, which it deliberately does not.

## Decision

A new inherited field, `tcb.agent_depth`:

```
child->agent_depth = (parent->is_agent || parent->agent_depth > 0)
                   ? parent->agent_depth + 1
                   : 0;
```

- A process outside any agent lineage stays at 0 and is unaffected.
- The first child of an agent is depth 1; its child 2; and so on.
- `SYS_FORK` returns `-EAGAIN` when `current->agent_depth >= SPAWN_DEPTH_MAX` (3).

So an agent may found a chain three deep, and the fourth generation is refused.

## Why this does not touch the shell

The cap is scoped to agent lineages precisely so ordinary process trees are
untouched. `init` → `prism` → a command are all `agent_depth == 0`, because none
of them is an agent and none descends from one. A global depth cap would have
risked breaking pipelines, which fork more than the obvious once — and breaking
the shell to bound agents would be fixing the wrong thing.

## `-EAGAIN`, not `-EPERM`

The caller is *permitted* to fork; it has hit a resource ceiling. `-EPERM` would
tell an agent author their capability is wrong and send them looking for a grant
that does not exist. `-EAGAIN` says what is true: not now, not this deep.

## The gate — `smoke-spawndepth`

An agent forks a chain, each generation forking again. The gate requires:

1. depths 1, 2 and 3 all succeed — the cap does not fire early
2. the **fourth** generation's fork returns `-EAGAIN` — the cap fires at all
3. the refusal happens at depth 3 specifically, reported by value, so a cap that
   fires at the wrong depth fails rather than passing as "something was refused"

Arm 1 matters as much as arm 2: a cap that refuses everything also "bounds
replication", and would pass a gate that only checked for a refusal.

## The rule this earns

**A limit must be keyed on something the operation propagates.** Authority is
deliberately not inherited across fork; lineage is. Keying a containment control
on the non-inherited property produces a control that passes its own test and
contains nothing.
