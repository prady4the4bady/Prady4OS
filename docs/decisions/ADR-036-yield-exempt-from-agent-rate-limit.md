= ADR-036 — `SYS_YIELD` is exempt from the agent syscall rate limit

**Status:** ACCEPTED. **Supersedes the counting scope of ADR-026 D7.**
ADR-026 D7 otherwise remains in force verbatim — window size, budget, kill
semantics, non-agent exemption and log line are all unchanged.
**Date:** 2026-08-13
**Evidence base:** DDR-915.

## What ADR-026 D7 said

> The syscall dispatcher enforces a **60 syscalls / 1 s sliding window per
> process** for agent processes. […] Exceeding the budget **cleanly kills** the
> offending process and logs `AGENT_RATE_LIMITED PID=N`. […] This bounds a
> tight-loop agent's ability to DoS the single core.

The counter admitted **no exceptions**: every syscall an agent issued counted,
`SYS_YIELD` included.

## The defect this caused

`SYS_YIELD` is voluntary turn-surrender. An agent calling it is doing the exact
opposite of monopolising the core — it is handing the core back. Counting it
against a budget whose stated purpose is "bounds a tight-loop agent's ability to
DoS the single core" penalises the behaviour the policy exists to encourage.

Concretely, in `user/actiondagtest.c` an agent waiting for a peer polls a shared
key and yields between attempts: 2 syscalls per iteration
(`SYS_MEMORY_READ` + `SYS_YIELD`). It therefore crossed the 60-syscall budget at
roughly **30 poll iterations** and was killed — *regardless of whether its peer
was cooperating perfectly*. The kill is silent from the agent's perspective:
`sched_exit(137)` never returns to the handler, so a correctly-behaved waiting
agent simply vanishes mid-rendezvous.

Two independent confirmations, both recorded in DDR-915:

1. **Kill code path** — `kernel/syscall/syscall.c:94-96`: the check is
   unconditional, agent-only, and calls `sched_exit(137)` before dispatch.
   The peer sovereign is exempt purely because `is_agent` is false, which is the
   >160x progress asymmetry measured between two structurally identical loops.
2. **Runtime log** — `build/artifacts/dagdiag-20260813T024223Z.log` contains
   `AGENT_RATE_LIMITED PID=29`, the DAG agent, on the failing run.

This is not specific to one test. **Any** future agent that must wait on a peer
will hit it, which is why the fix belongs in the policy and not in the caller.

## Decision

**`SYS_YIELD` (NSI 3) does not increment the agent rate-limit counter.**

The exemption is **narrow and enumerated**, not a category: it applies to
`SYS_YIELD` alone, because `SYS_YIELD` is the only syscall in this kernel whose
entire semantic is "surrender the CPU and make no other request". Every other
syscall an agent issues — **including `SYS_MEMORY_READ`, and including blocking
calls that do real work such as reads, waits on file descriptors, and IPC** —
continues to count exactly as before.

If a future syscall is added whose whole purpose is likewise to give up CPU with
no other effect, adding it here requires a further ADR. The enumeration must not
be widened by inference from this one.

## Why this does not weaken the protection

The threat D7 addresses is an agent consuming the core in a tight loop. That
threat requires the agent to *do work* — issue real syscalls, or spin without
syscalls at all. Neither is affected:

- An agent spamming any real syscall (`SYS_MEMORY_READ`, writes, IPC) still
  crosses 60/s and is still killed, unchanged.
- An agent spinning in userspace without syscalls was never caught by D7 in the
  first place; preemption handles it, and this ADR changes nothing there.
- An agent calling only `SYS_YIELD` in a tight loop surrenders the core on every
  iteration by construction. It cannot starve a peer — yielding is precisely
  the concession the scheduler needs to run someone else.

So the set of behaviours that can DoS the core is unchanged; only the set of
behaviours that are *falsely* punished shrinks.

## Verification bar (met — see DDR-915)

A two-arm test is REQUIRED to accept this change, because an exemption that
quietly gutted the limiter would pass arm A alone:

- **Arm A** — the cooperative busy-poll rendezvous (`smoke-actiondag`) must pass
  deterministically once `SYS_YIELD` is free.
- **Arm B** — an abusive agent spamming a *counted* syscall with no yields must
  still be killed with `AGENT_RATE_LIMITED`.

Arm A alone proves only that the limiter stopped firing. Arm B is what proves it
still fires for the right reason.
