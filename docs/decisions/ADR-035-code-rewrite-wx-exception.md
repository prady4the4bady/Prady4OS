# ADR-035 — a bounded, sovereign-gated exception to W^X for code rewriting

**Status:** proposed — **binding once accepted**.
**Date:** 2026-07-31
**Supersedes:** nothing. **Amends the scope of ADR-021 (W^X)** by carving out one
narrowly-bounded exception, and does not weaken it anywhere else.
**Governs:** work item E-05 (`SYS_APPROVE_CODE_REWRITE`). **No E-05 code may be
written before this ADR is accepted** — CLAUDE.md holds that a binding ADR may
only be superseded or amended by another ADR, never quietly by code.

## Why this ADR exists at all

ADR-021 establishes W^X: no page in this system is ever simultaneously writable
and executable. E-05 asks for a syscall that lets a process overwrite a code
page. Those two statements cannot both hold unqualified.

The wrong resolutions, named so they are not reached for later:

* implementing E-05 and treating W^X as "mostly true" — that is amending a
  binding ADR by code, which the project rules forbid, and it makes the
  invariant unauditable because nothing states where it stops holding;
* declaring E-05 out of bounds — the owner has asked for self-modifying agent
  code (§F MOSS pipeline, F#67 self-healing, F-05 rewriting), and every one of
  those depends on it.

So the invariant needs an explicit, bounded exception with a stated threat model,
which is what an ADR is for.

## Decision

W^X continues to hold for **every page, at every moment**, with exactly one
exception, and every clause below is part of the boundary:

1. **Both credentials, not either.** The caller must hold `CAP_SOVEREIGN` **and**
   invoke `SYS_APPROVE_CODE_REWRITE`. `CAP_SOVEREIGN` alone grants nothing here —
   otherwise every sovereign process would silently carry the power to rewrite
   code, which is precisely the ambient authority W^X exists to deny.
2. **One page.** A single named code page, identified by virtual address, is
   made writable. Not a range, not "the process's text", not a page count
   argument.
3. **Bounded by the call.** The page is `W+X` only between the mapping and the
   syscall's return. The kernel restores `R+X` **before** returning, on every
   path including error paths. There is no interface that leaves a page writable
   across a syscall boundary, and therefore no state in which a W^X violation
   can be observed by any other thread scheduled after the call.
4. **Audited before, not after.** The audit record — caller pid, target VA,
   size, timestamp — is written **before** the mapping is granted. If the record
   cannot be written, the mapping is refused. An audit trail written afterwards
   would be missing exactly the entries that matter: the ones where the process
   did not survive the window.
5. **No other page changes protection.** The call may not map new pages, extend
   an existing mapping, or alter permissions on any page other than the named
   one.

## Threat model — what an attacker inside the window can and cannot do

Assume a process holding `CAP_SOVEREIGN` is fully compromised at the moment it
calls `SYS_APPROVE_CODE_REWRITE`. Stated plainly, because an exception without a
threat model is a hole with paperwork.

**Can:** write arbitrary bytes into that one page, and thereby execute arbitrary
code *in that page* afterwards. This is the granted capability, not a failure of
it — a rewrite primitive that could not change behaviour would be pointless.

**Cannot:**

* extend the window — the restore is on the kernel's return path, not requested
  by the caller, so there is no input that lengthens it;
* widen the blast radius — one page, named in advance; no other page's
  protection is touched;
* escape audit — the record precedes the grant, so an attempt is recorded even
  if the process is killed mid-window;
* acquire the capability — `CAP_SOVEREIGN` is not grantable by this call, so
  this is not a privilege-escalation primitive, only a
  privilege-*exercise* primitive;
* reach any other process — the mapping is in the caller's address space only.

**Residual risk, stated rather than minimised:** a compromised sovereign process
can rewrite its own code. That is inherent in the feature the owner asked for.
The mitigation is not technical but procedural: `CAP_SOVEREIGN` is owner-held,
and §F-05's sovereign reasoning gate requires a non-empty rationale record before
the action is granted, so the audit log carries *why* every rewrite happened.

## What this ADR does NOT authorise

* No live kernel patching. This applies to ring-3 code pages. Kernel text stays
  RX permanently (DDR-757 `vmm_protect_kernel`), and nothing here relaxes it.
* No W+X page that outlives a syscall.
* No blanket "sovereign processes may rewrite code" rule — the explicit call is
  required every time, for every page.

## Gate obligations for E-05

The E-05 gate must assert the **boundary**, not merely that the rewrite works:

* a caller with `CAP_SOVEREIGN` but no `SYS_APPROVE_CODE_REWRITE` call cannot
  write the page;
* the page is `R+X` again after the call returns — read the PTE, do not infer it;
* the audit record exists **before** the write is observable;
* a second page, adjacent to the target, is unchanged in protection.

`FORBIDDEN_SENTINEL: CODEREWRITE_STUB`. Per S11 the gate is absent rather than
stubbed if the PTE cannot actually be inspected from the gate.
