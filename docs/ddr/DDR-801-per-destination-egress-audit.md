# DDR-801 — R3: the allowlist *match* path has no per-destination audit

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-29
**Closes:** DDR-794 risk **R3**, a precondition for enabling `cloud_bridge`
(DDR-793).
**Builds on:** DDR-800, which added `ACTION_NET_CONNECT` and `AETHER_DEST_ID`.

## The gap

After DDR-800 the egress audit covers two of the three outcomes:

| outcome | recorded before this slice |
|---|---|
| denied (no `CAP_NET`, or destination off the allowlist) | ✅ `AR_CAP_DENIED` + destination |
| allowed **by the sovereign bypass** | ✅ `AR_SOVEREIGN_BYPASS` + destination |
| **allowed by policy** (`CAP_NET` + allowlist match) | ❌ **nothing** |

So the ordinary, expected, most frequent case — an agent connecting to a host
the operator explicitly permitted — left no record at all.

That is the wrong way round in the same way R1 was. The log could answer "what
was refused" and "what the operator overrode", but not "what actually went out".
For a fixed local Ollama endpoint that is tolerable; DDR-794 flagged it as
untenable for `cloud_bridge`, where the destination set is unbounded and the
whole point of an audit is to be able to review, afterwards, where data went.

`netallow_check()` returning 0 is a decision. An undocumented decision is not an
audit trail.

## Decision

Emit `AR_NET_CONNECT` with the destination on the **success** path — every
connect that policy allowed.

The result is that all three outcomes are now recorded, with the same
`ACTION_NET_CONNECT` action type and the same `AETHER_DEST_ID(host, port)`
encoding, so a reader can filter the log by action type and get the complete
egress history rather than a biased sample of it.

### Placement — after the authority decision, before the socket

The record is emitted once the decision is made and before `psock_connect()`
runs, deliberately:

* **not after** `psock_connect()`, because a connect that is authorised but then
  fails on `-EMFILE` or a dead network is still an *egress attempt that policy
  permitted*, and that is the thing being audited. Logging only what succeeded
  at the TCP level would silently drop authorised attempts and make the trail
  depend on network conditions.
* **not before** the checks, because that would record attempts that were
  refused as though they were allowed — the denial path already has its own
  record.

### Why not one record with a status field

Considered. Three distinct result codes cost nothing (the enum already exists)
and make the common queries trivial — "show me everything the operator
overrode" is a filter on one value rather than a compound predicate. More
importantly it keeps DDR-800's property intact: *allowed by policy* and *allowed
by operator authority* must never be indistinguishable.

## Gate

`make smoke-egress-audit` — one probe, two connects, both asserted from the
audit log via `SYS_READ_AUDIT`:

1. to `192.0.2.1:9999`, which the kmain self-test installs on the allowlist →
   expect `AR_NET_CONNECT` carrying that destination;
2. to `192.0.2.1:9998` (**wrong port**, same host) → expect `AR_CAP_DENIED`
   carrying *that* destination.

The second is what makes the gate discriminating on the encoding as well as the
path: same host, different port, so an implementation that records the host but
drops the port produces two identical `action_id`s and cannot satisfy both
assertions. A gate that only checked the allowed case would pass against a
hard-coded destination.

The probe runs **non-sovereign with `CAP_NET`**, so neither record can come from
the DDR-800 bypass path — otherwise the gate would pass on the wrong evidence.
