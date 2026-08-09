> **SUPERSEDED IN PART by DDR-878 (2026-08-09).** The per-gate rate here was
> measured from suite-level reds. Running each gate individually on one pinned
> SHA gives a different and much narrower picture: `smoke-rqstress-liveness`
> fails ~1/8, and the four block-layer `-smp 4` gates were clean 32/32. The
> flake is one gate, not "the SMP gates".

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

## AMENDED — a denominator became available, so a rate can now be stated

The first version of this DDR declined to give a reproduction rate, correctly:
the occurrences had no recorded denominator. Continuing to push produced one,
and a **fifth** intermittent gate (`smoke-smp`, run 31153802543 — also
`QEMU_SMP=4`).

Every full-suite run since the CMake bugs were fixed, so the denominator is
clean — these are not my failures:

| SHA | branch | verdict | gate |
|---|---|---|---|
| `e51584a` | dev/phase1 | FAIL | `smoke-crosswake` |
| `e51584a` | main | FAIL | `smoke-crosswake` |
| `8a2754c` | main | **PASS** | — |
| `8a2754c` | dev/phase1 | FAIL | `smoke-blkmq-trace` |
| `f6dfa0f` | main | **PASS** | — |
| `f6dfa0f` | dev/phase1 | FAIL | `smoke-smp` |

**4 of 6 initial full-suite runs failed — every one on an `-smp 4` gate**, and
every re-run on unchanged code has passed.

A per-run failure probability of ~0.67 across the ~20 SMP gates in a suite
implies a per-gate flake probability of roughly `1 - (0.33)^(1/20) ≈ 5.4%`.
That inference assumes the gates fail independently, which is unproven — if they
share one root cause they will not. Treat ~5% as an order of magnitude for a
single SMP gate, and **~67% per full suite as the directly observed figure**,
which needs no modelling at all.

## This blocks item 50, which the queue did not anticipate

Item 50 requires **three consecutive green CI runs on one tip** before
`dev/phase1` → `main` promotion.

At an observed ~33% pass rate per run, three consecutive greens is `0.33³ ≈ 3.6%`
per attempt — roughly **28 attempts expected**, at ~30 minutes each. That is not
a slow path to release; it is a closed one.

**So item 47 is not an optional "fix or document" item — it gates item 50.**
The queue lists them as independent, and on this evidence they are not. Flagged
rather than worked around: raising the promotion rule to "3 greens allowing
re-runs" would satisfy the letter of item 50 while removing exactly the signal
it exists to provide.

## What is still deliberately NOT claimed

**No claim that all five share one root cause.** They share a configuration.
Whether that is one bug or several living in the same place is what a targeted
campaign would separate — one pinned SHA, N runs of the five gates individually
rather than through the full suite, so a per-gate rate is measured rather than
inferred from a suite-level number.
