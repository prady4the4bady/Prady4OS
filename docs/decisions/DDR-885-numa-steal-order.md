= DDR-885 — NUMA-affine work-stealing order (Group 6 item 37)

**Status:** Accepted — implemented; node-preference NOT gate-proven (see §4)
**Date:** 2026-08-10
**Depends on:** DDR-882 (item 17) for `numa_node_of_cpu()`.
**Scope:** `kernel/proc/sched.{c,h}`, `kernel/main.c`, `smoke-rqstress`.

## 1. What changed

`rq_steal()` is now two passes: same-node victims first, then everything else.
Stealing moves a thread's working set to another CPU; doing that within a node
keeps its memory local, doing it across nodes makes every later access remote.

## 2. The second pass is unconditional, and that is the important part

A CPU that finds no local work still steals remotely. An idle CPU next to a
loaded one on another node is worse than a remote access, and a scheduler that
let a runnable thread sit in the name of locality would not be an optimisation —
it would be a liveness bug. The hint biases order; it never withholds work.

## 3. Only the ORDER changed — placement did not

Which runqueue a thread is *enqueued* on is deliberately untouched. That would
change placement, and this subsystem is under investigation for an intermittent
lost-thread defect (DDR-880, DDR-884). Changing placement now would make the next
red impossible to attribute — the same reasoning that kept DDR-864's fix on hold
until there was a measured baseline.

## 4. What the gate proves, and what it does NOT

Measured on a 4-CPU single-node machine after the rqstress storm:

```
[sched] steal local=86850 remote=0
```

`remote=0` shows the first pass is doing all the work rather than falling
through, and `smoke-rqstress` stays green, so the two-pass path schedules
everything.

**The node PREFERENCE is not proven.** Every CPU on that machine is node 0, so
"same node" is trivially true. Proving preference needs 4 vCPUs across 2 nodes,
and that combination is currently unreliable: the NUMA gates run `-smp 2` and
rely on DDR-785 early exit specifically to avoid the unresolved lost-thread
defect. This waits on that defect, exactly as item 50 does.

**And the assertion is weaker than it looks.** The gate matches
`[sched] steal local=`, which also matches `local=0`. A mutation that skips the
same-node pass entirely was killed (rc=2) — but the kill was most likely the
pre-existing `rqstress FAIL` forbidden pattern, not this assertion. **I did not
confirm which check killed it**, and a mutant killed by the wrong assertion is
the same trap DDR-881 hit with `kill %n`. The line is therefore documented as a
DIAGNOSTIC, not as a gate with teeth, until a two-node 4-CPU machine can assert
`remote=` meaningfully.

**Item 37 ships implemented and honest, not implemented and overclaimed.**
