# DDR-1002 — §8.2 tested in BOTH directions: a two-arm `smoke-evresize` campaign

**Status:** PRECOMMIT — thresholds and stopping rules fixed BEFORE any run.
**Supersedes nothing. Extends:** DDR-989 (the fix), DDR-994 (the detector),
DDR-1000 §8 (the prediction), DDR-990 §4 (the both-directions standard).

---

## 1. What this measures, and — first — what it does NOT

This tests **DDR-1000 §8.2** and nothing else:

> Inflated `vruntime` starves whoever holds `mnt_lock`; a waiter then spins in
> an unbounded `yield()` loop forever. If that is right, **DDR-989's fix removed
> the trigger** and organic `mnt_lock` stalls should no longer occur.

**It does not bear on OPEN-1 route 1.** DDR-1000 §8.1 already settled that and
the point is easy to lose, so it is restated here as the first line of the file:

> Route 1 is `smoke-surfdestroy` hanging inside `sys_read`; this is
> `smoke-evresize` failing a resize round-trip with the guest still alive. Same
> lock, different gate, different symptom.

Route 1 is additionally **CI-only** and this campaign is **local**, which is the
second, independent reason a clean result here cannot close it — the same reason
DDR-1000 §9.3 refused to let 60 clean `smoke-surfdestroy` logs close it.

A session note written before this file framed the evresize campaign as "the
OPEN-1 route 1 test". That framing was wrong and is corrected here.

## 2. Why one arm is not enough — the flaw in the campaign already running

At the time of writing, `smoke-evresize` stands at 6/60 clean on kernel
`60b35c96d70253f5`, zero `[yieldstall]` lines of any kind.

A clean sweep of arm A alone is **the same under-powered claim DDR-1000 §3
rejected for route 2** — worse, in fact, because route 2 at least had a
*measured* base rate (1/20, DDR-985) to compute power against. The organic
evresize stall has **no measured rate at all**: it is known from two captures
(`build/gatelogs/lf-smoke-evresize.out` pid=45,
`build/gatelogs/vrj-6.out` pid=43), neither of which establishes a denominator.

So N=60 clean on the fixed kernel would license exactly one sentence — "it did
not happen in 60 boots" — with no power figure attachable to it. §NON-NEGOTIABLE
17 wants a denominator and there is none.

The fix is not more runs. It is a second arm.

## 3. The mutation withdraws exactly one property

DDR-989's fix is one load:

```c
    uint64_t in  = t->vt_in;                      /* fixed:  ONE read      */
    uint64_t d   = (now > in) ? (now - in) : 0;
```

The mutant restores the torn read and changes nothing else:

```c
    uint64_t d = (now > t->vt_in) ? (now - t->vt_in) : 0;   /* MUTANT: two reads */
```

This satisfies the DDR-990 §4 standard on every count:

- **One property withdrawn.** Single-read atomicity of `vt_in`. The `g_dbg_floor`
  clamp, `rq_pop`'s stamping, the runqueue, `mnt_lock`, and the `[yieldstall]`
  detector are all untouched.
- **The mutant self-identifies.** `g_vrjump_n` / `g_vrjump_d` (DDR-989 §8.4) stay
  compiled in, so a mutant boot that actually tears prints `vrjn>0` in its
  heartbeat. A mutant run with `vrjn=0` did **not** exercise the defect and is
  recorded as such rather than counted as a clean observation.
- **Distinct kernel hashes are mandatory.** Arm A is `60b35c96d70253f5`. If the
  mutant build hashes identically, the build did not take and the run is void
  (R1). That check has already earned itself once this project — DDR-990 §9's
  first mutant build failed while `kernel.bin` still held the fixed hash.

## 4. Arm B — measure the rate (N_B = 20)

Kernel: mutant. Gate: `smoke-evresize`. Ledger: separate from arm A's.

**Counting rule, fixed now.** A boot counts as a hit iff
`tools/ci/yieldstall_scan.py` reports an **organic unresolved** stall for it:
a `[yieldstall] site=mnt_lock` line with `pid != 0` and no `RESOLVED` partner.
`pid=0` is `smoke-yieldstall`'s own synthetic arm (DDR-994) and never counts.
Gate PASS/FAIL is **not** the measurement — the pre-fix captures failed the gate,
but the stall is the observable, and a mutant boot could stall without failing.

`k_B` = hits out of 20.

## 5. Precommitted decision rules — written before the data exists

**If `k_B = 0`:** the mutation does not reproduce the trigger locally at N=20.
§8.2 is then **UNTESTABLE BY THIS DESIGN**, and that is the finding. It is
recorded as a null and arm A's clean runs are **not** converted into a power
claim. This DDR precommits to reporting that outcome rather than re-tuning the
mutant until something fires — a mutant adjusted after seeing a null result is
no longer a test of the hypothesis that motivated it.

The leading explanation for a null is worth naming in advance so it is not
invented afterwards: the mutant is **today's kernel minus DDR-989**, not the
historical kernel the two captures came from. That tree also lacked DDR-981's
interrupt window and DDR-987's `g_net_lock`, among others. If the organic stall
needed one of those absences as well, reverting DDR-989 alone will not reproduce
it — and that would mean §8.2 names a *composite* trigger, of which DDR-989 is
only one term. A null is therefore informative about the design, not only about
the rate.

**If `k_B >= 1`:** let `r_lo` be the Clopper–Pearson 95% one-sided lower bound on
`k_B/20`. Then

```
N_A = ceil( ln(0.05) / ln(1 - r_lo) )
```

is the number of clean arm-A boots needed for 95% power against the *conservative*
end of the measured rate. Arm A runs to `N_A`, on ONE kernel hash.

**Cap.** If `N_A > 60`, the campaign stops at 60 and reports the power actually
achieved rather than the power desired. The deadline is 2026-08-31; an honest
partial figure beats an unfinished campaign.

**Refutation.** Any organic unresolved stall in arm A **refutes §8.2**: the
`mnt_lock` wait would then be independently unbounded, and bounding it becomes a
fix with a named mechanism behind it (which §NON-NEGOTIABLE 3 currently forbids,
precisely because no such artefact exists yet).

## 6. Arm A's existing 6 runs — the condition for keeping them

Arm A's 6 completed runs are on `60b35c96d70253f5`. Building the mutant and
restoring afterwards means arm A's kernel is rebuilt. Those 6 runs count toward
`N_A` **iff the restored build hashes to `60b35c96d70253f5` exactly**. If it does
not, arm A restarts from zero. A campaign spanning two kernels is not a campaign
(R1), and this session has already discarded four runs to that exact mistake.

## 7. Cost

Arm B is 20 runs at ~2 min = ~40 min. If `r` is high the derived `N_A` is small
and the two-arm design is *cheaper* than the 60-run single arm it replaces, as
well as strictly stronger. If `r` is low, `N_A` is capped at 60 and the cost
equals the original plan plus arm B.
