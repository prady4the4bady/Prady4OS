# DDR-800 — R1: the sovereign egress bypass is total and unrecorded

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-29
**Closes:** DDR-794 risk **R1**, a precondition for enabling `cloud_bridge`
(DDR-793).
**Relates to:** DDR-731 (`CAP_NET`), DDR-734 (egress allowlist), ADR-027 (socket
NSI).

## The audit

Every site where `is_sovereign` short-circuits a network authority decision,
traced against the tree rather than recalled:

| # | site | what it bypasses | audited today |
|---|---|---|---|
| 1 | `sys_socket.c:75` — `if (!is_net && !is_sovereign)` | the `CAP_NET` authority check | **no** — the branch is not taken, so nothing is logged |
| 2 | `sys_socket.c:81` — `if (!is_sovereign && netallow_check(...))` | the DDR-734 egress allowlist, entirely | **no** — same |
| 3 | `sys_socket.c:67` — `sock_denied()` | another process's socket slot | **no** |
| 4 | `sys_socket.c:168` — `sys_net_allow` | nothing; sovereign is *required* to install a rule | n/a — correctly gated |

Sites 1–3 are bypasses. Site 4 is the opposite — an authority requirement — and
is listed so a later reader does not "fix" it.

## The actual finding

The bypass is not the problem. An operator needs unrestricted egress for
diagnostics, and DDR-734 says so deliberately.

The problem is that **it leaves no trace**. `aether_audit(..., AR_CAP_DENIED)`
fires only on the *denial* path, so:

* a normal agent denied egress produces a record;
* a sovereign thread reaching *any host on the internet* produces **nothing**.

The one category of egress with no limits is also the one category with no
evidence. After an incident, the audit log cannot answer "did anything sovereign
call out, and where to" — it can only report the calls that were refused. That
is precisely backwards.

It also silently widens: `is_sovereign` is set in exactly two places
(`main.c:424` for the ELF path, `main.c:1258` for the daemon), and any future
third site inherits unrecorded unrestricted egress with no signal.

## Decision — keep the exemption, record it

The exemption stays; it is intentional and bounded to kernel-granted sovereign
threads. What changes is that taking it becomes an **audited event with its own
result code**, distinguishable from an ordinary authorised connect:

```c
AR_SOVEREIGN_BYPASS      /* new: egress proceeded ONLY because is_sovereign */
```

Emitted when, and only when, the sovereign flag is what allowed the call —
i.e. the thread lacked `CAP_NET`, or the destination was not on the allowlist,
or both. A sovereign thread that *would* have been allowed anyway is recorded as
an ordinary connect, so the code means what it says: "this call happened because
of operator authority, not because it was permitted."

The destination travels in `action_id` as `(host_be << 16) | port`, using the
existing record shape. No new syscall, no wider audit struct — the fields are
already there and unused on this path.

### Why not close the bypass instead

Considered and rejected. Removing it would mean the operator cannot diagnose the
network without first installing an allowlist rule through a path that itself
requires network diagnostics to debug. The bypass is load-bearing for recovery.
An unrecorded bypass is the defect; an unusable operator is not the fix.

### Why a distinct result code rather than reusing `AR_APPROVE`

`AR_APPROVE` would make a sovereign call to an arbitrary host indistinguishable
from an allowlisted call by an ordinary agent. The whole value of this record is
that the two are different, so they get different codes.

## Gate

`make smoke-sovereign-egress` — a sovereign-flagged kernel probe with
`is_net = 0` attempts a connect to a host that is **not** on the allowlist. It
must:

1. succeed (the exemption still works), and
2. produce an audit record with `AR_SOVEREIGN_BYPASS` **carrying the
   destination**.

An implementation that keeps the bypass but forgets the record passes (1) and
fails (2) — which is exactly the defect this DDR exists to close, so the gate
discriminates rather than merely exercising the path.
