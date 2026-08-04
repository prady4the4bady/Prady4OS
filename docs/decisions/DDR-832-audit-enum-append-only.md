# DDR-832 — `enum aether_result` is append-only wire format

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/aether/aether.h` `enum aether_result`
**Caused by:** DDR-814 (AGS wiring)
**Family:** the recurring structural defect — a duplicated constant with nothing checking the copies agree

## What happened

Wiring AGS added two audit codes. I inserted them **in the middle** of
`enum aether_result`, immediately after `AR_WRAP`, because that grouped them near
the other capability-ish codes and read well.

CI went red on **three shards at once**, each on a different gate:

```
shard 1: FAILED at smoke-privacy-netfilter after 8 of 18 gates
shard 2: FAILED at smoke-egress-audit     after 4 of 18 gates
shard 4: FAILED at smoke-sovereign-egress after 5 of 33 gates
```

None of those gates has anything to do with goal signing.

## Mechanism

The audit `result` codes are **wire format**: they travel across the ring
boundary inside audit records, and ring-3 probes cannot include a kernel header,
so they duplicate the numbers as literals:

```c
/* user/egressaudittest.c */
#define AR_SOVEREIGN_BYPASS  9
#define AR_NET_CONNECT      10
```

Inserting two codes after `AR_WRAP` shifted `AR_SOVEREIGN_BYPASS` 9 -> 11 and
`AR_NET_CONNECT` 10 -> 12. The probes then asserted against stale numbers, found
no matching record, and failed. The kernel and the probes were each internally
consistent and disagreed with each other, and **nothing in the build noticed**.

## Decision

1. `enum aether_result` is **append-only**. New codes go at the END.
2. Every value that is duplicated outside this header is **pinned with a
   `_Static_assert`**. A renumbering now fails the *build*, at the line that
   caused it, instead of surfacing as three unrelated red gates on three shards.

## Why not the alternatives

- **Just move the codes to the end and move on.** That fixes today's breakage and
  leaves the trap armed for the next person who groups an enum tidily.
- **Give the probes the real header.** They are freestanding with their own code
  model; pulling in `aether.h` drags the kernel's include graph into ring 3. The
  duplication is deliberate — what was missing is a check that it stays true.
- **Assign explicit numbers to every enumerator.** Reasonable, but it makes the
  invariant implicit again: nothing would stop someone renumbering. The asserts
  state the contract out loud, with the reason attached.

## The rule this earns

**A constant duplicated across a boundary needs a check that both copies agree,
not a convention that they should.** The kernel enum and the ring-3 literal were
each locally correct; correctness lived in the *relationship* between them, and
nothing was testing the relationship. This is the same shape as DDR-831 (a
constant encoding another subsystem's size, enforced only by a comment) and
DDR-830 (a free list invariant enforced by nothing) — and it is why the checks
have to be executable.

## Note on process

This was caught by CI on the first run after the change, before the feature was
marked shipped, because the tracker rows said "awaiting first CI conclusion"
rather than ✅. The ordering did its job.

## Addendum — DDR-833, found while verifying this fix

The first verification run of the fix reported the three gates STILL failing.
They were not: `make image` printed **"Nothing to be done"** and the gates ran the
previous binary. `$(KERNEL_BIN)` listed `$(KERNEL_CS)` (sources) but **no kernel
headers**, so editing `aether.h` rebuilt nothing.

The comment directly above that rule already documents this exact failure for
`user/` (DDR-822) and `kernel/crypto/` (DDR-825), ending with *"the wildcard
fixed user/ and stopped there."* It stopped one directory short a third time.

Fixed by adding `KERNEL_HS` (all `kernel/**/*.h`) to the prerequisites. Had I
trusted the first run, I would have concluded the enum fix did not work and gone
looking for a second, non-existent bug — which is precisely how the two wrong
OPEN-11 root causes happened.
