= DDR-936 — an unblocked thread never runs for 65 s on a HEALTHY single CPU

**Status:** MEASUREMENT. No fix in this slice — the mechanism is not yet known.
**Date:** 2026-08-16
**Evidence:** CI run 31926397044 (tip `cf3146c`), shard 3, `smoke-agent-click`.
**Lineage:** DDR-930/932/934 (blk `done=0x0`) — **same signature, new subsystem.**

## The measurement

`smoke-agent-click` failed with the clicked PRAX agent never starting. The
serial log, read against the `[hb] t=` heartbeats:

```
line 331:  PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82
line 332:  [hb] t=5000 thre_drops=0 rx_drops=0 spins=0 max=0 cpu=0 calls=0 bails=0
line 339:  [hb] t=5500 …
…
line 351:  [hb] t=11500 …        (and continuing)
```

So after the agent was spawned:

- **the system ran ~6500 more ticks (~65 s), healthily** — ticks advancing,
  `spins=0`, `thre_drops=0`, `rx_drops=0`, `bails=0`;
- **PRAX never printed `PRADYOS_AGENT_START`** — not late, never.

## What this rules out

**Not the gate window.** §8's standing rule says check elapsed against the
window before reading code. The window is 120 s and the trigger landed with
~65 s of measured, healthy runtime after it. This is not "no time to run".

**Not allocation** (DDR-934's hypothesis for the blk case). The hook returns
`ut->pid`, and the log carries `pid=82`, so `elf_load` produced a real TCB.
Allocation succeeded. DDR-934's `spawned<total` branch does not apply here.

**Not a cross-CPU wake / missing IPI** (DDR-932's proposed mechanism, already
refuted in DDR-934 on other grounds). The `smoke-agent-click` QEMU line has
**no `-smp` flag at all** — this is a single-CPU boot. There is no AP to fail
to wake, no per-CPU runqueue to strand work on, and no IPI in the path.

That is worth stating plainly because it is the third time this mechanism has
been proposed for this failure class. On a uniprocessor it is not available as
an explanation.

## What the code says happened

`aether_spawn_agent_hook` (`kernel/main.c:856-868`):

```c
if (elf_load(…, &ut) != ELF_OK || !ut) return -1;
ut->is_agent = 1;
ut->is_net   = 1;
ut->parent_pid = g_aether_daemon_pid;
sched_unblock(ut);                 /* elf_load returns the thread BLOCKED */
return (long)ut->pid;
```

Every step before the return is known to have executed, because the return
value reached ring 3 and was printed. So the narrowed question is exactly:

> On a single CPU, with the timer demonstrably firing for 65 s, how does a
> thread that has been through `sched_unblock()` never get selected?

## The unified observation

This is the same shape as the blk-probe `done=0x0` failures (DDR-930/932/934):
**a thread is created successfully and never executes its first instruction,
while the system remains healthy.** Two unrelated subsystems (AETHER agent
spawn, virtio-blk probe workers) showing one signature is evidence for one
defect in the create/unblock/enqueue path rather than two coincidental ones.

DDR-934's decision table for the blk case listed
`spawned=total` + `prog=0` ⇒ "genuinely scheduling". This log is the agent-side
instance of exactly that branch, arriving before the blk instrument reported —
and it arrives on a uniprocessor, which is a much smaller search space.

## Next step — read, do not guess

Read `sched_unblock()` and the enqueue path for a case where the TCB is marked
runnable but not pushed to the runqueue (or pushed to a queue the uniprocessor
scheduler does not scan). Confirm against the code before proposing a fix.

**Explicitly not doing:** proposing a mechanism in this document. DDR-920, 928
and 932 each named a mechanism from inference and each was refuted by later
evidence. This slice records what was measured and stops there.

## Status of `smoke-agent-click`

OPEN intermittent. Passes 3/3 locally; failed CI run 31926397044. It is NOT
clear for the three-greens promotion count.
