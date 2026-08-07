# DDR-863 — every intermittent gate is an `-smp 4` gate (Group 9 item 47)

**Status:** Accepted — analysis, no code change
**Date:** 2026-08-07
**Scope:** Evidence for item 47 (`-smp 4` virtio-blk defect, B#3/DDR-806) and
OPEN-2.

## The observation

Four gates have failed intermittently and been triaged rather than fixed:

| gate | occurrences | source |
|---|---|---|
| `smoke-resched` | 1 | DDR-828 (2026-08-03) |
| `smoke-blkmq-trace` | **2** | DDR-828, plus run 31149744394 (2026-08-07) |
| `smoke-msixap` | 1 | run 31139497587 (2026-08-07) |
| `smoke-crosswake` | 2 | runs 31142981014 / 31142982460 (2026-08-07) |

**All four run `QEMU_SMP=4`.**

## Why that is a signal and not a shrug

The base rate matters, so it was measured rather than assumed:

- **138** gates total
- **20** use `QEMU_SMP=4` — **14.5%**
- **118** are single-CPU, and **none has ever flaked**

If intermittency were spread uniformly across gates, four independent flakes all
landing inside a 14.5% subset is roughly `0.145⁴ ≈ 1 in 2,300`.

That figure carries an explicit assumption — that flakes would otherwise be
uniform across gates — which is a simplification: SMP gates are also longer and
more IO-heavy, so they have more opportunity to fail for unrelated reasons. It
is offered as an order of magnitude, not a p-value. The qualitative claim stands
without it: **zero of 118 single-CPU gates have flaked, four of 20 SMP gates
have.**

## Two clean demonstrations of intermittency

Both from this session, and both stronger than the earlier single-occurrence
triages:

1. **Same SHA, re-run flips it.** `smoke-crosswake` failed on `e51584a`, and a
   re-run of that shard on the identical tree with **no code change passed**.
2. **Same SHA, two branches, opposite outcomes.** `8a2754c` was pushed to both
   branches. `main` passed; `dev/phase1` failed at `smoke-blkmq-trace`. Same
   commit, same code, different verdict.

Neither is an argument that something *should* be flaky. They are observations
that it *is*.

## What this changes

Item 47 was framed as "the `-smp 4` virtio-blk bug — fix or document". The
evidence now supports a stronger, testable statement:

> The intermittency is a property of the **`-smp 4` configuration**, not of the
> individual gates. Four unrelated gates — a scheduler gate, a block-queue
> trace, an MSI-X/APIC gate and a cross-CPU wake — share no logic beyond running
> on four CPUs.

That is one hypothesis to falsify instead of four separate mysteries, and it
points the investigation at the shared substrate: AP timer/tick handling,
per-CPU state, and the virtio-blk queue under concurrent CPUs — the same area
DDR-777/DDR-806 already implicate.

## What is deliberately NOT claimed

**No reproduction rate.** Item 47 asks for a measured one, and 6 occurrences
across an unrecorded denominator is not a measurement. Producing a percentage
from it would be inventing precision.

A real rate needs a deliberate campaign: one pinned SHA, N runs of the four
gates, occurrences counted. That is the correct next step for item 47 and is
bounded work — 20 runs of four gates at ~180 s each is roughly 4 h of QEMU, and
it would either produce a rate or fail to reproduce, both of which are answers.

**No claim that all four share one root cause.** They share a configuration.
Whether that is one bug or several living in the same place is exactly what the
campaign would begin to separate.
