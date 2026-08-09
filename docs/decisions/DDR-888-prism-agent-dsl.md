= DDR-888 — PRISM agent DSL (Group 6 item 36)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `user/prism.c`, `smoke-shell`.
**Context:** ADR-024 §D3 deferred the agent DSL without specifying it, so this
DDR defines it.

## The verbs

```
agent list                          -> AGENT ROSTER slots=N active=M
agent spawn <path> [task]           -> SYS_SPAWN_AGENT   (CAP_AGENT)
action submit <type> [payload]      -> SYS_SUBMIT_ACTION (agents only)
action poll <id>                    -> SYS_POLL_RESULT
action approve <id>                 -> SYS_APPROVE_ACTION (CAP_SOVEREIGN)
```

The action **type is parsed from the argument** rather than fixed. A DSL that
hard-coded one type could not express the Section 3C action set, which is the
thing this surface exists to reach.

## The interesting property: PRISM is unprivileged, and that is the test

PRISM runs with neither `CAP_AGENT` nor `CAP_SOVEREIGN`. Three of the five verbs
are therefore **expected to be refused**, and the refusal printing is the
assertion:

```
prism> AGENT ROSTER slots=8 active=0
prism> AGENT SPAWN DENIED rc=-1
prism> ACTION SUBMIT DENIED rc=-1
prism> ACTION APPROVE DENIED rc=-1
```

`agent list` works because `sys_agent_roster` is deliberately ungated —
observability is not a privileged operation. The other three hit
`is_agent`/capability checks in `sys_aether.c` and return `-EPERM`.

A shell that offered `agent spawn` and silently did nothing would look like a
missing feature. One that *appeared to succeed* would be a capability hole
presented as a working command. Printing the denial, with its `rc`, is the only
honest option — and it is what the gate checks.

## Mutation testing, including the part that went wrong first

Two mutations were run and both "survived" — then the file was checked and the
`sed` had matched **nothing**, because the patterns contained `\n` escapes that
did not survive quoting. A mutation that never applies reads exactly like a
mutant the gate cannot kill.

The re-run **verifies the edit applied before trusting the verdict**:

| Mutation | Applied? | Result |
|---|---|---|
| rename the spawn-denial line so it never prints | verified yes | **killed** — `FAIL: agent spawn NOT denied without CAP_AGENT` |

That is the general lesson, and it now has three instances in this project: a
mutation harness must prove the mutation exists before reporting what it means.
The first two attempts are recorded as **invalid, not as survivals**.

## Scope

Not implemented: `agent kill` (SYS_KILL_AGENT exists but a shell that cannot
spawn cannot meaningfully own an agent to kill), payload encodings beyond a raw
string, and result formatting per action type. Each needs the Section 3C type
table to be settled first, and inventing an encoding here would create a second
one to reconcile later.

**Group 6 item 36 complete.**
