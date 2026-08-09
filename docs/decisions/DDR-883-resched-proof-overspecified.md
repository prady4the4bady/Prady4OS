= DDR-883 — the resched proof failed a correct system at `-smp 2` (found via item 17)

**Status:** Accepted — test defect, fixed
**Date:** 2026-08-09
**Scope:** `kernel/main.c` (`smpresched_proof`).
**Found by:** item 17's NUMA gate, which is the first thing in this project ever
to boot with `-smp 2`.

## The finding

`[smp] resched FAIL` reproduced **5/5 at `-smp 2`** and **0/5 at `-smp 3` and
`-smp 4`**. Deterministic, and specific to exactly two vCPUs.

This is **not** the items 46/47 lost-thread defect. That signature is a proof
that never prints at all. Here the proof runs and prints FAIL — a different
failure mode with a different cause.

## The evidence that settled it

The proof reported one bit. Instrumented to report both terms, `-smp 2` gives:

```
[smp] resched FAIL ipis=0 ran=1
```

**`ran=1`.** The unblocked thread ran. The system was correct; the test was
wrong.

`smpresched_proof` required `g_resched_ipis > before && g_rp_ran`. With one AP
the unblocked thread is picked up without a cross-CPU kick, so no resched IPI is
sent — and the proof failed a system that had done exactly what was asked of it.

This is the recurring shape in a new place: an assertion over an **implementation
detail** (an IPI was sent) standing in for the **property** (the thread runs).
The two coincide at 3 and 4 CPUs, which is why every existing SMP gate passed and
nothing caught it. `-smp 2` had simply never been run.

## The fix, and why it is not just a weakening

The property is asserted unconditionally: **the thread must run**. The IPI term
is retained where a cross-CPU kick is genuinely the expected path — more than one
AP, `lapic_cpu_count() > 2` — so the resched IPI keeps its coverage at exactly
the configurations every existing SMP gate uses.

Both terms print on failure, because "never ran" and "no IPI needed" demand
opposite actions and one bit cannot tell them apart.

**Teeth verified rather than assumed.** With `g_rp_ran` forced to 0 — a genuine
liveness failure — the gate fails at **both** `-smp 2` (`ipis=0 ran=0`) and
`-smp 4` (`ipis=1 ran=0`). The relaxation did not remove the assertion's ability
to fail. Per the standing rule, the check was tested against a case it must fail,
not only cases it must pass.

Verified: `-smp 2`, `-smp 3`, `-smp 4` all 0/3 failures after the fix.
`smoke-resched`, `smoke-crosswake`, `smoke-smp` green. Zero warnings.
