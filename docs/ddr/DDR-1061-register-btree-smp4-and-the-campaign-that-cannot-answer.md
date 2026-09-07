# DDR-1061 — `smoke-sfs-btree-smp4` registered on its exclusion's own condition, and the rate campaign that cannot settle it at any reachable N

**Status:** REGISTERED (shard 5). Campaign STOPPED as null-on-design, deliberately.
**Date:** 2026-09-05
**Relates to:** DDR-964 (the OPEN-10 fix), DDR-824 (the reproduction surface),
DDR-1023 (local route exhausted), DDR-1002 (the null-on-design precedent),
DDR-1060 §9/§10 (the campaign tool).

---

## 1. The exclusion's condition is met, and it is a condition about a MECHANISM

`tools/ci/shard_check.sh` has carried, since DDR-824:

> `smoke-sfs-btree-smp4` ) DDR-824 OPEN-10 reproduction surface. Registering it
> now would make CI red on a known-open defect and block unrelated promotions.
> **Register it when OPEN-10 is fixed.**

OPEN-10 is fixed. **DDR-964** named the mechanism rather than describing the
symptom — `rc=-1` is `-EPERM` from `cap_ok(cap, CAP_FS_WRITE)`, because
`sched_create()` made a thread runnable *before* its caller minted the capability
into `->arg`, so a thread picked up early ran with `CAP_NULL`. Eight sites were
converted to `sched_create_blocked()` → set arg → `sched_unblock()`, and the fix
was mutation-checked.

The condition the exclusion states is **"OPEN-10 is fixed"**, not "a campaign has
bounded the rate". That distinction is the whole of this DDR.

## 2. The rate campaign CANNOT answer the question at any N reachable here

I ran one anyway, and stopped it on the arithmetic rather than on a result.

**Power, computed before continuing** (0 failures in *n* runs; 95% upper bound on
the per-run failure rate is `1 − 0.05^(1/n)`):

| n | 95% upper bound | P(0 in n \| p = 0.067) |
|---|---|---|
| 3 | 63.2% | 0.812 |
| 10 | 25.9% | 0.500 |
| 20 | 13.9% | 0.250 |
| 30 | 9.5% | 0.125 |
| **44** | **6.6%** | 0.047 |
| 60 | 4.9% | 0.016 |

The pre-fix rate `open10_campaign.sh`'s own header records is **2/30 = 6.7%**. So
**n = 44 merely *reaches* the historical rate** — it does not clear it with any
margin, and it certainly does not distinguish "fixed" from "still 6.7%", which
would need far more. At the measured **182 s per run** (timed cleanly this
session), 44 runs is ~2.3 hours of *foreground* execution.

**And foreground is the only option.** DDR-1060 §10 records it: three background
campaigns were each killed after 1–5 runs when the session went idle, `setsid`
included — this container executes only while a turn is live. So the campaign is
not merely expensive, it is expensive *and* has to be driven by hand in chunks.

**Conclusion: the instrument cannot answer the question.** That is DDR-1002's
shape — a campaign null on its own design — and the difference worth carrying is
that DDR-1002 discovered it *after* spending the runs and this one discovered it
*before*. **Do not run this campaign. It has been costed and it does not pay.**

## 3. What was actually measured

**3 runs, 0 failures, on kernel `c33afa79f60abdcb`**, hash-verified before AND
after every run by DDR-1060 §9's pin, all recorded in one report
(`build/artifacts/open10-20260904T223618Z.txt`). Plus one bare
`make smoke-sfs-btree-smp4`, rc=0 in 182 s, not counted (different invocation, no
pin).

**3/3 bounds the rate below 63% and that is all it does.** It is reported here so
nobody mistakes its absence for an untried experiment, not as support for the
decision. The decision rests on §1.

## 4. The decision, and the residual risk stated plainly

**Registered on shard 5** (lightest: 16 gates / 1376 s, going to 1556 s — still
below shard 9's 1965 s), 180 s, `strict`.

**The risk is real and is not hidden:** if the defect is not in fact fixed, every
CI suite becomes measurably more likely to be red, which directly degrades the
§NON-NEGOTIABLE 1 three-green criterion the release depends on.

**Why register anyway:**

- The exclusion's own stated condition is met, and it pre-authorises exactly this.
- A gate held out of the matrix is coverage the project is not getting for a
  defect it believes fixed — and this repository's recurring lesson (DDR-1046,
  DDR-1049, DDR-1060) is that a control nobody exercises rots unnoticed.
- **If it reddens, that IS the measurement** — a real CI failure with a capture
  is worth more than any number of clean local boots, and it is the evidence the
  campaign provably cannot produce.
- Un-registering is one line, and the release is HELD on an operator decision, so
  there is no promotion in flight for a red to block.

**NOT CLAIMED:** that the defect is proven gone. Three clean runs prove nothing
of the sort, and this DDR does not lean on them. What is claimed is narrower —
the mechanism was named and fixed and mutation-checked (DDR-964), the exclusion
asked for exactly that, and the rate evidence the exclusion did *not* ask for is
unobtainable here.

## 5. If it does redden

Do **not** re-run the local campaign; §2 is why. Read the CI capture instead:
`rc=-1` with `btree churn FAIL` and no `[sfs] btree churn OK` reproduces the
DDR-964 shape, and the capability state at the failure is the discriminator —
DDR-964's fix is about *when* `->arg` is minted, so a recurrence means a
`sched_create()` site was missed, not that the analysis was wrong. Un-register
and reopen OPEN-10 with the capture attached.
