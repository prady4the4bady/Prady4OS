# DDR-1064 — THE DISCRIMINATOR HAS THE SAME RACE IT WAS BUILT TO SETTLE

**Status:** FINDING recorded + DDR-1030 CORRECTED (it contradicts itself) +
instrument FIX designed. **NO scheduler defect is claimed and none is fixed.**
**Date:** 2026-09-06
**Trigger:** CI 34003737145, shard 4, `smoke-smppreempt`, tip `e9ed2c9` —
`[smp] resched FAIL ipis=0 ran=1 idle=1 idle2=1`.
**The first real `idle2=` reading ever produced.** Every prior appearance of that
field in this repo is DDR-1030's own design text or its forced mutant.

---

## 1. What the artefact is, and what it is not

`e9ed2c9` changes **three `.md` files and nothing else**, and the shard's own
post-gate assertion printed `kernel.bin: OK` — so the binary is bit-identical to
the tip that was green, and this failure cannot have been caused by the commit it
landed on. `ran=1` also says **the property under test HELD**: the unblocked
thread ran. The IPI term is a stronger claim layered on top.

`resched FAIL` is in `GLOBAL_FORBIDDEN` (DDR-791), so it reddens whatever gate it
lands in — here `smoke-smppreempt`, which does not own the assertion. That is
working as designed and is not a second defect.

## 2. DDR-1030 CONTRADICTS ITSELF, and both halves are wrong

Its §5 table:

| reading | DDR-1030 §5 says |
|---|---|
| `idle=1 idle2=0` | sampling artefact |
| `idle=1 idle2=1` | **"a scheduler defect"** |

Its §6, four paragraphs later:

> `idle2=1` on a healthy boot is the expected reading, which is what makes
> `idle2=0` in a real failure informative.

Those are opposite. §6 got there by reading its own forced mutant, which ran with
`ipis=1` — the kick *was* delivered, so an idle AP at both instants is exactly
what a healthy boot shows. §6 generalised from a run whose `ipis` term differed
from the failure shape, so it is wrong as written; the FAIL branch's own
precondition is `ipis=0`.

**But §5 is wrong too, and that is this DDR's finding.**

## 3. The finding: idle2 has the SAME class of race, on the other side

The kernel kicks from **inside** `sched_unblock` (`kernel/proc/sched.c:1837`):

```c
for (int c = 0; c < PERCPU_MAX; c++) {
    struct percpu *o = percpu_get((uint32_t)c);
    if (c != self && o && o->present && o->idle) {
        if (smp_resched_one((uint32_t)c)) break;   /* DDR-1014: break on DELIVERED */
    }
}
```

The proof samples `idle_after` **after that call has already returned**
(`kernel/main.c:1031`). `o->idle` is a live per-CPU flag. So:

- **DDR-1004's race** (the one DDR-1030 set out to close): a CPU **leaves** idle
  between the first sample and the call → no kick owed → correct system prints
  `idle=1 ipis=0`.
- **DDR-1030's own race** (not named anywhere until now): a CPU **enters** idle
  between the call and the second sample → no kick was owed *when the kernel
  looked* → correct system prints `idle2=1 ipis=0`.

DDR-1030 closed one window and opened its mirror image. `idle2=1` therefore does
**not** establish that a kick was owed and missing; it narrows the timing but does
not close it. **This capture is consistent with a correct kernel.**

This is the DDR-1046 / DDR-1060 shape a third time: **a control that cannot see
the case it exists for.** DDR-1060's rule applies verbatim — an instrument proven
only on the healthy path (DDR-1030 §6's forced mutant reached `t=5000` on a
healthy boot) is proven only for the healthy path.

## 4. The conclusion has been copied into three documents

`docs/BUILD_TRACKER.md:1850`, `docs/PRE_LAUNCH_CHECKLIST.md:234` and
`:378` all state `idle2=1` = "the kick was owed and missing" / "a real scheduler
defect". All three are corrected here. A wrong reading in three places is how the
next session convicts the scheduler on this capture — and §NON-NEGOTIABLE 3 would
have been *satisfied on paper* while the mechanism was never named.

## 5. The fix: ask the kernel, do not paraphrase it

DDR-1014 already wrote the rule and applied it to the **predicate**:

> the two loops must ask the same question or the proof is not testing the
> kernel, it is testing a paraphrase of it

DDR-1014 made the predicates match (`!o->is_bsp`, because `smp_resched_one`
declines the BSP). **The INSTANT still does not match**, and that is the residual
one level down. The proof cannot fix this from outside, because every sample it
takes is at the wrong time by construction.

So `sched_unblock` records what **its own loop saw, at the only instant that
matters**, and the proof reads that instead of re-deriving it:

- `saw_idle` — an idle non-self CPU was visible **to the kernel's own loop**
- `kicked` — `smp_resched_one` actually **delivered**

**Stored on the TCB, not in a global.** A global would be clobbered by any other
CPU's unblock between the proof's call and its read — `sched_unblock` runs from
MSI-X interrupt context on the virtio-blk completion path (DDR-1014), so that is
not hypothetical. Keyed to the thread, the value cannot be stolen.

**§NON-NEGOTIABLE 10 applies:** `kmalloc` does not zero, so both fields need an
explicit initialiser in `sched_create` — the exact defect class §INV.6 exists for.

**Cost, stated:** two stores in the **CAS-succeeded slow path** of
`sched_unblock`, never on the fast path, and no `rdtsc`. This is deliberately not
the cost DDR-1047 refused (an `rdtsc` pair on every `spin_lock` acquisition):
that one could *move* OPEN-2 by adding work to the path where the race lives.
Two plain stores on a path that has just done a CAS, an `rq_push` and possibly an
IPI is noise beside what is already there.

## 6. The verdict does NOT change, deliberately

`resched FAIL` keeps its meaning and stays in `GLOBAL_FORBIDDEN`. Collapsing this
to SKIP would green the gate and delete the coverage DDR-1014 built — the trade
DDR-1012 and DDR-973 each had to undo, and DDR-1030 §3 already refused once.
What changes is that the **next** occurrence carries a field whose reading is not
racy, so it can convict or exonerate rather than merely narrow.

## 7. NOT CLAIMED

- **No scheduler defect is named and none is fixed.** OPEN-1, OPEN-2, OPEN-12,
  OPEN-13 are untouched. This capture is consistent with a correct kernel and is
  **not** evidence of one.
- **This does not make the gate deterministic.** The FAIL can still fire on a
  correct system until the new field replaces the racy one in the verdict, and
  §8 does not do that — the verdict still uses the old terms, because changing a
  gate's verdict on the strength of one capture is how coverage gets deleted.
- **No rate is measured.** §4.10 of the checklist records that gap and this does
  not close it; one occurrence bounds nothing.
- **The new fields are not yet proven on a failing path** — the same limit
  DDR-1060 recorded about DDR-1047's M1. They are proven wired; they are not
  proven to read correctly during a genuine missed kick, because no such
  occurrence has been produced.
