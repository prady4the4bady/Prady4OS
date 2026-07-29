# DDR-802 — kernel-side privacy hook: block egress at the syscall boundary

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-29
**Closes:** the scope gap DDR-793 recorded — what shipped for CONFIRM-1 gate (3)
was the *Python transport boundary*, which is narrower than the gate's words.
**Relates to:** DDR-731 (`CAP_NET`), DDR-734 (allowlist), DDR-800/801 (egress
audit).

## The gap this closes, stated exactly

DDR-793's gate (3) asked for a hook that blocks the cloud path when privacy mode
is active. What shipped was `aether/platform/privacy/netfilter.py`, wrapping the
Python transport. That is the right place for `cloud_bridge` and
`ollama_bridge` — and it covers **nothing else**.

Every ring-3 process on the machine can call `SYS_SOCK_CONNECT` directly. PRISM
can. An agent can. The `capnettest` probe does. None of them go through Python,
so "privacy mode" was a property of one library rather than of the system.

A privacy control that only binds the callers who opted into it is a convention.
This makes it a property of the kernel.

## Decision — refuse before the allowlist is consulted

`sys_sock_connect` gains a privacy check **first**, ahead of the `CAP_NET`
authority check and the DDR-734 allowlist:

```
privacy active?  -> audited -EPERM, return           <- new, DDR-802
CAP_NET check    -> audited -EPERM
allowlist check  -> audited -EPERM
audit the allowed connect                             <- DDR-801
psock_connect
```

Ordering is the substance of this DDR, not an implementation detail:

* **Before the allowlist.** If the allowlist ran first, an allowlisted
  destination would be *permitted* and then blocked, which makes the two
  mechanisms interact: an operator reading the log would see a policy decision
  that did not govern the outcome. Privacy mode means "nothing leaves", so it
  must be evaluated before any question about *where* something is going.
* **Before the `CAP_NET` check.** Same reason in the other direction: a process
  with no `CAP_NET` and a process with it must be refused identically while
  privacy is on. Making the refusal depend on a capability the caller does not
  need would leak whether it holds one.
* **Ahead of the sovereign bypass.** Deliberate, and the one place this DDR
  overrides DDR-800. The sovereign exemption exists so an operator can diagnose
  *the network*; privacy mode is the operator's own explicit instruction that
  nothing leaves the machine. Honouring the bypass here would let the flag
  override the very control the operator just set. The refusal is audited with
  the sovereign's pid, so the attempt is still visible.

## State

One kernel flag, `g_privacy_mode`, set through the existing sovereign-only
`SYS_SET_MODE` path — no new syscall. It is read at call time, never cached: a
value captured elsewhere would go stale exactly when the operator flips it,
which is the same reasoning the Python hook already applies.

## Audit

The refusal emits `ACTION_NET_CONNECT` + the destination with a new result code
`AR_PRIVACY_BLOCKED`. Distinct from `AR_CAP_DENIED` because "refused by policy"
and "refused because privacy mode is on" are different operator-facing facts,
and collapsing them would make it impossible to answer "did privacy mode
actually stop anything?" — which is the only question that establishes the
control works in production rather than in a test.

## Gate

`make smoke-privacy-netfilter` — a probe with **`CAP_NET` and an allowlisted
destination**, i.e. a connect that would otherwise unambiguously succeed:

1. connect with privacy **off** → allowed, `AR_NET_CONNECT` recorded;
2. privacy **on** → same destination now returns `-EPERM` with
   `AR_PRIVACY_BLOCKED` recorded;
3. privacy **off** again → allowed once more.

Using an allowlisted destination is what makes this discriminating: a probe
aiming at a blocked host would get `-EPERM` either way, and the gate would pass
against an implementation that does nothing. The third step catches a hook that
latches on and never releases — a privacy mode that cannot be turned off is a
different bug, not a stricter version of this one.

## Implementation — state, and why not a bitmask

`aether_set_mode()` already existed but coerced its argument to a single
boolean, `g_sovereign_mode`. Widening it to a bitmask (bit 0 sovereign, bit 1
privacy) was the obvious move and is **wrong here**: `aether_get_mode()` is
returned verbatim to ring 3 by `SYS_GET_MODE`, and userspace compares that
result against literal `0`/`1` (`user/compositor.c:837`, `user/aether_daemon.c`).
OR-ing a privacy bit into it would silently break those comparisons — a
correctness bug in the UI introduced by a security fix.

So privacy is a **separate** flag, `g_privacy_mode`, reached through the same
sovereign-gated `SYS_SET_MODE` (no new syscall, as decided above) using two
additional selector values, `AETHER_MODE_PRIVACY_ON/OFF` (2/3). The 0/1 contract
is untouched, and privacy stays on its own axis — it is not a third kind of
sovereignty, and turning egress off must not drop the machine out of sovereign
mode as a side effect.

Blast radius was enumerated before the edit, not assumed: `g_sovereign_mode` has
exactly one internal consumer (`aether_queue.c`, the approval gate) and
`aether_get_mode()` exactly one caller (`sys_aether.c` → `SYS_GET_MODE`). Both
are unaffected because the historical coercion is preserved for every value
other than 2 and 3.

## Gate — DEFERRED, and why (this slice ships the mechanism only)

The mechanism above is complete and dormant: `g_privacy_mode` defaults to 0, so
no existing gate changes behaviour. **`smoke-privacy-netfilter` is deliberately
NOT shipped in this slice**, and the probe `user/privacynettest.c` is written but
not wired into the build.

The reason is a race I could not remove cheaply. Every probe in `kmain` is
spawned with `sched_unblock` and runs **concurrently**; `user_boot_from_sfs`
writes-then-loads unconditionally, so there is no way to make a probe run in
only one gate. Privacy mode is global kernel state. A probe that switches it on
— even for the two syscalls this one needs — can refuse the concurrent connects
in `capnettest`, `sovegresstest` and `egressaudittest`, failing *their* gates at
random.

That is the `rtcmonotest` mistake exactly: a probe whose cost lands on unrelated
gates, with a window small enough to pass locally and hit eventually in CI. A
two-syscall window is still a race, and "small window" is what BUG-1 punished
for four hypotheses.

**What the gate needs first:** a kernel-visible per-boot opt-in, so the privacy
probe exists only in its own QEMU configuration. The nearest precedent is
`QEMU_SFSROOT`, which the kernel detects via an extra block device rather than
through any general mechanism — so this needs designing, not copying. That
design is the first step of the next slice, ahead of the gate and ahead of the
three-arm A/B (remove the check / remove the audit emission / drop the distinct
result code — each must fail the gate).

Shipping the mechanism without its gate is the deliberate choice over shipping a
gate that destabilises three neighbours. Per S11 the gate is **absent**, not
passing — there is no stub asserting success.
