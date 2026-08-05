# DDR-839 — DAG action queue: `parent_action_id` and dependency-ordered approval

**Status:** accepted
**Date:** 2026-08-06
**Governs:** `kernel/aether/aether_queue.c`, `kernel/syscall/sys_aether.c`, NSI 92
**Section:** E

## What this is

Actions may declare a parent, turning the flat queue into a dependency graph:

```
SYS_SUBMIT_CHILD_ACTION  92  (type, payload, len, parent_action_id)  CAP_AGENT
```

**A child cannot be approved before its parent is.** An agent that plans "fetch
the data, then write the report" can submit both up front and rely on the kernel
to refuse the write until the fetch is actually approved — instead of the
operator having to remember the ordering, or the agent having to re-submit.

## Cycles are structurally impossible, not merely rejected

`action_id` is monotonic and never reused within a boot. A parent must already
exist to be named, so `parent_action_id < action_id` always holds. A cycle would
require an action to name a parent allocated after it, which cannot happen.

That is worth more than a cycle check: a validator can be wrong, and this cannot.
No cycle-detection code exists here **because none is reachable**, and that is a
property of the id allocator, so anyone who makes ids reusable or non-monotonic
breaks this and must revisit it. Recorded here so that link is not lost.

## A NEW syscall, not a fourth argument to NSI 31

`SYS_SUBMIT_ACTION` (31) is declared `(type, payload, len)` and ignores its
fourth argument register. Threading `parent_action_id` through that unused
register would be free — and wrong: every existing caller leaves that register
**undefined**, so old binaries would submit actions with a garbage parent id.
Most would fail `-ESRCH`; some would attach to a real unrelated action.

So NSI 92 is a separate call, and NSI 31 keeps its exact meaning. An unused
argument register is not a spare field; it is a field whose value nobody has
been required to set.

## Rules

- `parent_action_id == 0` means "no parent" — a root action, behaving exactly as
  NSI 31 does today.
- A parent that does not exist (or has been recycled out of the ring) →
  `-ESRCH` at submit time. Failing at submit is better than accepting an action
  that can never be approved.
- Approving a child whose parent is not `AE_APPROVED` → `-EAGAIN`. Not `-EPERM`:
  the operator has the authority, the dependency is simply unmet.
- A rejected or expired parent needs **no cascade code**: its children can never
  reach `AE_APPROVED` because the check above never passes. Cascade logic would
  be a second mechanism that has to agree with the first.

## The gate — `smoke-actiondag`, five arms

1. submit a root, then a child naming it — both accepted
2. approve the **child first** → `-EAGAIN` (the ordering is enforced at all)
3. approve the parent, then the child → both succeed (the ordering is not simply
   "always refuse", which arm 2 alone would accept)
4. submit naming a nonexistent parent → `-ESRCH`
5. reject a parent, then try to approve its child → still refused, with no
   cascade code in the kernel

Arms 2 and 3 are a pair on purpose. A queue that refuses every child approval
passes arm 2 and is useless; one that ignores parents entirely passes arm 3 and
enforces nothing.

## The rule this earns

**An unused argument register is not a free extension point.** Its value is
undefined precisely because no caller was ever obliged to set it, so reading it
turns every existing call site into a source of garbage. New meaning needs a new
entry point.
