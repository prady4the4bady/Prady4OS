= DDR-891 — capability-based service manager in PID 1 (Group 3 item 15)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `user/init.c`, `smoke-init`.
**Replaces:** init's direct-reap model (2c.11).

## What was there

PID 1 called `wait4(-1, WNOHANG)` in a loop, printed what it collected, and did
nothing else. That is a *reaper*, not a service manager: nothing started
services, nothing knew what a service was, and nothing reacted to one dying.

## The model

A bounded static table. Each service declares a name, a path, a **restart
policy**, and the capabilities it is allowed to run with:

```c
{ "exectest", "/EXECTEST.ELF", RESTART_NEVER,       CAP_NONE }
{ "missing",  "/NOSUCH.ELF",   RESTART_ON_FAILURE,  CAP_NONE }
```

Start is `fork` + `execve`; supervision happens in the existing reap loop, which
now **matches the reaped pid to a service** and applies that service's policy
instead of only logging.

## Restarts are BUDGETED, and that is the important part

`RESTART_ON_FAILURE` with no bound is a fork bomb written by the supervisor. A
service whose binary is missing fails `execve` instantly, exits 127, gets
restarted, fails instantly again — PID 1 spins at 100% forever and the failure
looks like a hang rather than a misconfiguration.

So every service has a restart budget (3). On exhaustion the manager **gives up
and says so**:

```
[svc] giveup missing after 3 restarts
```

Giving up loudly is the whole point. A supervisor that silently stops retrying is
indistinguishable from one that never noticed, and a supervisor that retries
forever converts a broken config into a system-wide livelock.

## Capability gating

A service may only be started with capabilities PID 1 itself holds. init holds
none of the privileged ones, so a service declaring `CAP_AGENT` is **refused
before the fork**:

```
[svc] refuse agentsvc: requires caps init does not hold
```

This is checked in the manager rather than left to the kernel's `execve`,
deliberately. The kernel would refuse the privileged *operation* later, at first
use — by which point a process is running that should never have started, and the
failure appears somewhere unrelated to the misconfigured service.

## What the gate proves

`smoke-init` requires all four:

| Sentinel | Proves |
|---|---|
| `[svc] start exectest pid=` | a service is actually launched |
| `[svc] exit exectest` | the reap loop attributes an exit to its service |
| `[svc] refuse agentsvc` | capability gating rejects before forking |
| `[svc] giveup missing after 3 restarts` | the restart budget is bounded and reports |

The `giveup` line is the one with teeth: it can only appear if the manager
started the service, saw it fail, restarted it exactly three times, and then
stopped. An unbounded retry loop never prints it, and neither does a manager that
gives up on the first failure.

## Scope

**Not implemented:** dependency ordering between services, restart backoff delay,
`RESTART_ALWAYS` for long-lived services (nothing in this queue has one that
should be restarted on clean exit), socket activation, and runtime
start/stop/status control from the shell.

Runtime control is the one worth naming: it needs a syscall or an IPC endpoint
into PID 1, and inventing one here would create a second control surface to
reconcile with the PRISM agent DSL (DDR-888). It belongs with whichever of those
is designed first.

**Group 3 item 15 complete.**

---

## Observed lifecycle

```
[svc] start exectest pid=43
[svc] start missing pid=44
[svc] refuse agentsvc: requires caps init does not hold
[svc] exit exectest pid=43 st=0
[svc] exit missing pid=44 st=127
[svc] restart missing 1/3
...
[svc] restart missing 3/3
[svc] exit missing pid=47 st=127
[svc] giveup missing after 3 restarts
```

`agentsvc` is refused **before** any pid exists — no `start` line for it.

## Mutation matrix

| Mutation | Applied? | Result |
|---|---|---|
| remove the restart budget (retry forever) | verified yes | **killed** |
| remove the capability check | verified yes | **killed** |

Both were confirmed present in the source before the verdict was read. M1 is the
important one: without the budget the `giveup` line never prints, which is
exactly the livelock the budget exists to prevent — and the gate sees its absence
rather than waiting for a timeout to notice PID 1 spinning.
