# DDR-1132 — AN ABBREVIATED SHA IN THE HUNT'S `ref` INPUT VOIDS EVERY LANE, AND THE INVERTED POLARITY REPORTS THAT AS A FIND

**Status:** measurement + correction. **Docs-only in this commit**; no code change, no
gate, `kernel.bin` NOT rebuilt. **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE**,
no open issue moves (OPEN-1/2/12/13 untouched). `GLOBAL_FORBIDDEN` 77, 179 gates, no
new sentinel, no new gate.

**The defect is mine, in how I invoked the hunt — not in the kernel, and not in the
workflow's logic.** It is recorded because it is a **false-hit producer in the one
instrument OPEN-2 depends on**, and because it is the DDR-1130 family one step across.

---

## 1. What happened, measured

Two `workflow_dispatch` hunts were fired on 2026-09-21 at `lanes=20 runs=20 hunt=32`
against pinned kernel `ca8107ec7f5d8de7`.

| run | `ref` passed | lanes | result |
|---|---|---|---|
| 35581509323 | `dev/phase1-seyp3n` (branch) | 20/20 success | **400 boots, `signal_runs=0 churn_runs=20` per lane** |
| 35582316759 | `4b0c18d` (**7-char SHA**) | 20/20 **failure** | **ZERO boots** |

Run 35582316759 returned `conclusion=failure`. **On this workflow that means it found
something** — the file's own header says so in as many words (*"IT FAILS WHEN IT FINDS
SOMETHING … Do NOT fix it to pass on a hit"*), and DDR-1127 §2 built its whole reading
discipline on that inversion.

**It found nothing. Not one QEMU started.** From lane 0's log:

```
git fetch --no-tags --prune --depth=1 origin +refs/heads/4b0c18d*:refs/remotes/origin/4b0c18d* +refs/tags/4b0c18d*:refs/tags/4b0c18d*
The process '/usr/bin/git' failed with exit code 1
  ... (two retries, 18 s and 10 s) ...
##[error]The process '/usr/bin/git' failed with exit code 1
##[warning]No files were found with the provided path: build/gatelogs/open2hunt
```

`actions/checkout` resolves a `ref` that is not 40 hex characters as a **branch or tag
name**, so it fetched the refspec `refs/heads/4b0c18d*`, which matches nothing. The
checkout step failed, every later step was skipped, and the artifact step confirms the
campaign never wrote a capture.

**Dispatch 2 contributes ZERO boots to any count in this project.** It is not a clean
run, not a signal, and not a denominator.

## 2. Why this is the DDR-1130 family and not a one-off slip

DDR-1130's finding was that the campaign **asserts one precondition and not the other**:
the workflow checks the pinned kernel hash (DDR-1097 §6) because a stale binary would
"report clean forever, stably wrong", and nothing checked that the data disks existed,
which has the identical failure shape. §6.2 then shipped the data-disk assertion.

This is the same shape moved **one step earlier, to before the campaign script exists on
disk**:

* the kernel-hash precondition is asserted **by the campaign**, which requires a checkout;
* the data-disk precondition is asserted **by the campaign**, which requires a checkout;
* **that the checkout itself resolved what the caller meant is asserted by nothing** —
  and its failure mode is not a false *clean* but a false ***hit***.

So it is worse than DDR-1130's in one respect and better in another. Worse: DDR-1130's
failure produced a `signal_runs=0` that a later session could have pooled wrongly, which
is quiet; this one produces a `conclusion=failure` on the OPEN-2 hunt, which is the single
loudest signal this project has, and it is a lie. Better: it is **deterministic and total**,
which is exactly what made it cheap to catch — see §3.

## 3. The discriminator was free and needed no new code: 20 of 20

**A rare intermittent cannot fail every lane.** DDR-1128's real find was `signal_runs=1`
in **one lane of six**; DDR-1062 bounds the per-suite freeze rate below 6.9% and this DDR's
own §5 bounds the per-boot rate below ~1.2%. At any of those rates, twenty independent
lanes failing together has no plausible reading as a signal.

So the rule, which costs nothing and is available on every future hunt:

> **Before reading a hunt `failure` as a find, look at how many lanes failed.**
> One or a few → read the logs, it may be real. **All of them → it is a setup failure;
> find it before spending a minute on the kernel.**

That is what was used here, and the log confirmed it in one fetch.

## 4. A correction owed to the workflow's own comment — NOT SHIPPED, and why

`.github/workflows/open2-hunt.yml:60` says the `ref` input *"already accepts a full commit
SHA, so that case is already served."* The `ref` input's own `description` says
**"Branch to HUNT"**. Both are defensible readings of one field, and the gap between them
is exactly where this went wrong.

**What is measured here is only the 7-character case.** The 40-character case is
**claimed by that comment and NOT measured by me** — I did not spend a runner on it, and
I am not inheriting the claim as fact (DDR-1007's discipline). A one-lane one-run dispatch
would settle it for ~4 minutes of runner time; it is recorded as the cheap experiment and
deliberately deferred, because the operator's standing instruction this week is to spend
the CI budget on boots.

**THE REMEDY IS DESIGNED AND DELIBERATELY NOT SHIPPED**, and the reason is structural
rather than a judgment call: the failure happens **inside `actions/checkout`**, before the
harness exists on disk, so the only place a guard could live is the **workflow file** — and
that file lives on the **default branch `dev/phase1`** (DDR-1117: a
`schedule`/`workflow_dispatch` workflow anywhere else is unstartable by any means). Pushing
to any branch other than `dev/phase1-seyp3n` is forbidden to this session without explicit
permission, the same disposition DDR-1117 §6 took. **Measured and reported, not merged.**

The shape it would take, so it is not re-derived: a `setup`-job guard that fails fast with
a named reason when `ref` is neither resolvable as a branch nor a 40-character hex SHA —
in the shape of the `lanes` guard already in that job, and **through `env`, not a splice**,
per PR #23.

## 5. What dispatch 1 licenses, and the one number that is new

Dispatch 1's 400 boots and DDR-1128's 60 ran **the same binary**, `ca8107ec7f5d8de7`, so
they pool — and **only** because of that. DDR-1127's 0/60 is a different binary and its own
NOT CLAIMED forbids pooling it; that prohibition is respected here.

| | boots | signals |
|---|---|---|
| DDR-1128 (run 35504467004) | 60 | 1 |
| dispatch 1 (run 35581509323) | 400 | 0 |
| **pooled, binary `ca8107ec7f5d8de7`** | **460** | **1** |

Point estimate **0.217%** per boot; exact (Clopper–Pearson) 95% CI **[0.0055%, 1.205%]**.

**THE READING IS THE ONE DDR-1131 §4 PRE-REGISTERED, AND IT IS HELD TO RATHER THAN THE
FLATTERING ONE:** *"the rate is LOWER THAN ONE OCCURRENCE IN SIXTY SUGGESTED, not that the
occurrence did not happen."* `P(0 in 400 | p = 1/60) = 0.0012`, and the interval's upper
bound (1.205%) lies **below** DDR-1128's point estimate (1.67%), so the 1-in-60 *rate*
reading is excluded at 95% while **the event itself stands entirely unchallenged** — one
`[apfreeze]` with a resolved RIP and a per-CPU census (DDR-1128, DDR-1129) is not weakened
by later boots that did not reproduce it.

This is **the first single-binary rate estimate OPEN-2 has ever had.** Every prior figure
was either per-suite over many SHAs (DDR-1062) or a single-binary *bound with no events*
(DDR-1127).

**The practical consequence, stated because it changes what to do next:** at ~0.2% per
boot, an expected single occurrence costs ~460 boots and a 95%-confident catch costs
~1,400. The two dispatches sized for this week were sized against a 1-in-60 prior and are
**smaller than the problem**.

`churn_runs=20` on all 20 lanes is the load-bearing half of dispatch 1 and is why it counts
at all: every boot reached `rqstress_proof` and ran the create/exit churn the race requires,
so these are **400 non-vacuous boots** and DDR-1130's no-filesystem trap is not in play
(NON-NEGOTIABLE 17; DDR-1097 §7.2).

## 6. NOT CLAIMED

* **NO kernel defect is found and none is alleged.** `actions/checkout` is correct, the
  workflow's logic is correct, the campaign script is correct, and what was wrong is the
  **argument I passed**.
* **NO fix is shipped.** §4's guard is designed and explicitly not built, for a stated
  structural reason and not for lack of time.
* **The 40-character SHA case is NOT measured** and no claim is made about it beyond
  quoting the workflow's own comment.
* **NO rate is claimed for OPEN-2 in general.** §5 is one binary, one workload, `-smp 4`,
  `OPEN2_HUNT=32`, and must not be pooled across binaries — the discipline DDR-1062 and
  DDR-1127 each stated about their own numbers.
* **DDR-1128 IS NOT WITHDRAWN OR WEAKENED.** Its artefact, its RIP resolution, its per-CPU
  census and DDR-1129's halt-site attribution all stand; §5 corrects a **rate** inferred
  from it, which DDR-1128 itself never claimed (its own text says "NO RATE for OPEN-2 is
  claimed on this or any binary").
* **DDR-1131 IS NOT WITHDRAWN.** Its §3 readings are conditionals about a capture and are
  independent of N; only its §4 arithmetic input moved, and that is annotated at the site
  rather than rewritten (DDR-1110/DDR-1111's rule).
* **DDR-1130 IS NOT CRITICISED** — its §6.2 assertion is correct and did its job on
  dispatch 1, and this is a *different* precondition one step earlier in the chain.
* **NO artefact was produced for OPEN-2**, no `[schedcheck]` fired, and DDR-1129 §7 item 1
  (the owed field values) stays owed.
