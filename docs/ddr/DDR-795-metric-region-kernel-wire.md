# DDR-795 — the kernel↔Python metric-lockbox wire (F#68, item 11)

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-28
**Depends on:** F#68 (`aether/kernel/lockbox/metric_lockbox.py`, shipped),
IMP-C (vDSO shared page — the mapping pattern this reuses), ADR-021 (W^X).

## The problem this actually solves

F#68 makes the objective function immutable *to the Python layer*: a
non-`CAP_SOVEREIGN` principal cannot call `define()`/`supersede()`, and a direct
edit of `metrics.jsonl` is caught by the hash chain at load.

That leaves one hole, and it is the one that matters. **Both the content and the
hash chain live in the same place.** An attacker — or a sufficiently
self-interested agent — that can write the store can rewrite the entries *and*
recompute the chain over them. The chain proves internal consistency; it cannot
prove the history was not replaced wholesale, because nothing outside the file
attests to what the root should be.

So the wire is narrow and specific: **the kernel holds the root, the Python
layer holds the content.**

* The kernel owns one page containing the sealed Merkle root of the objective
  function, plus a generation counter.
* Every user address space maps that page **read-only, NX** (the IMP-C vDSO
  pattern, reused rather than reinvented).
* Ring 3 may read the root. A write faults — `#PF (present, write, user)` — and
  the kernel converts it to a clean process kill, exactly as `wxviol` already
  proves for text pages under ADR-021.
* The Python lockbox recomputes its own root and compares. Agreement means the
  store is the one the kernel sealed; disagreement means it is not, and no
  amount of rewriting the file can manufacture agreement.

## Why read-only mapping and not a syscall

A `SYS_METRIC_READ` syscall would work and would cost an NSI number. The page is
already the cheaper and stricter answer: a mapping cannot be forgotten at a call
site, has no argument to get wrong, and — the point — makes the *absence* of a
write path structural rather than a check somebody has to remember to perform.
It also keeps the read free, which matters because a verifier that costs a
syscall per check gets called less often than one that does not.

NSI stays at 75; nothing is added. That is a feature: the smallest syscall
surface that does the job.

## What this slice does NOT claim

The Python AETHER layer runs on the **host**, not in PradyOS ring 3. It does not
today read this page at runtime — the path from ring 3 to the host layer is the
AETHER daemon, and wiring that is a separate slice.

What ships here is: the kernel region exists and is genuinely unwritable from
ring 3 (gated), and both sides agree on the byte layout (gated on the Python
side against the same struct). Claiming a live end-to-end read would be claiming
something not built.

## Layout

One 4 KiB frame. Fixed-offset, packed, versioned — a layout change must bump
`version`, because a silent reinterpretation of these bytes is exactly the
failure this whole mechanism exists to prevent.

```
offset  size  field
0       4     magic       0x4D455452 ("METR")
4       4     version     layout version, starts at 1
8       8     generation  bumped on every sealed update
16      8     sealed_ts   kernel tick at seal
24      32    root        SHA-256 of the objective function (raw bytes)
56      4     entries     number of metric definitions covered
60      4     flags       bit 0 = sealed
64      ...   reserved, zero
```

## Gate

`make smoke-metric` boots a ring-3 probe that:

1. reads the magic/version/root from `METRIC_USER_VA` and prints
   `METRIC_READ_OK` — proving the page is mapped and readable;
2. then stores to it.

If the store faults, the kernel kills the process and the probe's next line
never runs. If the store *succeeds*, the probe prints `METRIC_WX_FAIL`, which
the gate declares forbidden. So the gate fails in the one way that matters and
cannot pass by accident of the probe crashing early — the read sentinel has to
appear first.

This is the same shape as the existing `wxviol` W^X regression, deliberately:
the two failures are the same class (a user write reaching memory that must be
read-only), and using one proven pattern for both means a regression in the
fault path shows up in two independent gates.
