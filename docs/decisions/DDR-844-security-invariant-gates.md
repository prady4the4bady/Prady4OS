# DDR-844 — `smoke-invariants`: S1–S8 as attack gates

**Status:** accepted
**Date:** 2026-08-06
**Governs:** `user/invarianttest.c`, `make smoke-invariants`
**Covers:** Group 2 item 13
**Release significance:** this is the gate the release-quality claim rests on

## Why this gate is different from every other gate

Every other gate asserts that a feature **works**. This one asserts that a set of
attacks **fail**. That inversion matters: S1–S8 have been prose in
`AETHER_MASTER_FEATURES.md` since Layer 6, and prose cannot regress visibly. A
capability check deleted by a refactor breaks no existing gate — the features all
still work; only the *refusals* stop happening, and nothing notices.

Each arm therefore performs a real attack from ring 3 and requires the kernel to
**reject** it. An arm that cannot fail is not an arm.

## The arms

| invariant | attack performed | required outcome |
|---|---|---|
| **S1** no self-escalation | an agent calls `SYS_SET_MEM_LIMIT` on **itself** to raise its own cap | refused |
| **S1** | an agent approves **its own** submitted action | refused (`-EPERM`) |
| **S2** bounded | submit a payload **larger than `AETHER_PAYLOAD_MAX`** | clamped or refused, **never a panic** |
| **S2** | `SYS_MEMORY_WRITE` a value larger than `MEM_MAX_VAL` | `-EINVAL` |
| **S2** | fork past `SPAWN_DEPTH_MAX` | `-EAGAIN` (already gated by `smoke-spawndepth`; asserted here as an invariant, not a feature) |
| **S4** human gate | an agent submits `ACTION_REWRITE_AGENT_CODE` and it is auto-approved in sovereign mode | must remain **PENDING** |
| **S5** append-only | ring 3 attempts to erase/rewrite the audit log | **no syscall exists** — asserted structurally, see below |
| **S6** fault isolation | pass a **kernel pointer** to a syscall's user-pointer argument | `-EFAULT`, kernel survives |
| **S6** | pass a **non-canonical** pointer | `-EFAULT`, kernel survives |
| **S8** skill/code veto | non-sovereign attempts code-rewrite approval | `-EPERM` |

## Two invariants asserted structurally, and why that is honest

**S5's "no user-space erase path"** cannot be attacked from ring 3, because the
absence of a syscall is not something a syscall can test. Asserting it requires
checking that no such entry point exists — which is a build-time property, not a
runtime one. It is covered by `ci-audit-noerase-check`, which greps the syscall
table for any registered erase/reset/clear entry point against the audit log and
fails the build if one appears. A runtime arm would have to invent the very hole
it is testing for.

**S3 and S7** (immutable objective function, verifier independence) depend on
F#66–72, which are **not built**. Their arms are deliberately absent rather than
stubbed to pass. A green arm for an unbuilt subsystem is worse than a missing
one: it converts "not implemented" into "verified". The gate's sentinel enumerates
which invariants it actually covered, so the coverage claim is visible in the log
rather than assumed from a green tick.

## The rule this earns

**A security invariant with no failing test is a comment.** Features are proven
by making them work; refusals are proven only by attempting the thing that must
be refused, and an invariant nobody attacks will regress silently the first time
someone refactors the check away.
