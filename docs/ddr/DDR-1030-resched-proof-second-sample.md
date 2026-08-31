# DDR-1030 — `resched FAIL ipis=0 ran=1 idle=1`: an instrument, not a fix

Status: **INSTRUMENT BUILT + mutation-proven. NO fix, and the verdict is
deliberately unchanged.**

---

## 1. The artefact

CI on `bdb41c7`, shard 3, `smoke-rqstress-liveness`:

```
[smp] resched FAIL ipis=0 ran=1 idle=1
```

The gate does not own that assertion — `resched FAIL` is a `GLOBAL_FORBIDDEN`
entry (DDR-791), so it reddens whatever gate is running when it prints. Shard 3
died at gate 1 of 21.

## 2. Not this PR's, and the evidence for that

- **The property under test HELD.** `ran=1`: the unblocked thread ran. The IPI
  term is a stronger claim about *mechanism*, layered on top.
- **The signature predates every change in this PR.** DDR-1004 built the
  three-outcome predicate; DDR-1014 fixed one cause of this exact shape (the
  proof counted the BSP as an idle candidate while `smp_resched_one` declines to
  kick it) and cites *"CI run on 72a474a, shard 5"* for it.
- **This PR cannot reach it.** `smoke-rqstress-liveness` boots with no
  virtio-tablet, so DDR-1026's press-edge latch never executes; DDR-1027's
  terminal and DDR-1028's sentinel are ring-3 and run long after SMP bring-up.
- **3/3 non-vacuous local PASS** on `086fb267171c136b` — the captures carry
  `[smp] resched OK`, not `SKIP`, so the arm was exercised.

## 3. The mechanism is already written down in the code

`main.c`'s own comment on the predicate:

> *"NOTE the residual race, stated rather than hidden: `idle_seen` is sampled
> just before `sched_unblock`, and a CPU can leave idle in between. That window
> is far narrower than the old unconditional assertion, but it is not zero — so
> a FAIL with `idle=1` is strong evidence and not yet proof."*

That is this artefact exactly. An AP that leaves idle between the sample and the
call is owed no kick, so `ipis=0` is correct behaviour and the FAIL is an
artefact of *when* the sample was taken.

## 4. Why one sample cannot settle it, and why this is not turned into SKIP

A genuinely broken kick prints the **same three fields**: the thread is picked up
by the next timer tick instead of the IPI, so `ran=1 ipis=0 idle=1` either way.

That is why the verdict is left alone. Collapsing this case to `SKIP` would make
the gate green and delete the coverage DDR-1014 built — the project has made that
trade badly before (DDR-1012's `smoke-horizon`, DDR-973's
`smoke-fat32-multicluster`), and both had to be undone by making the assertion
measure something real instead of relaxing it.

## 5. What was built

A **second idleness sample**, taken immediately after `sched_unblock` returns,
asking the identical question of the identical CPU (`self_idx` hoisted out of the
wait loop so the two loops cannot drift — the mismatch DDR-1014 had to fix once
already). Printed only in the FAIL branch:

```
[smp] resched FAIL ipis=N ran=N idle=N idle2=N
```

The next occurrence is then self-diagnosing:

| reading | meaning |
|---|---|
| `idle=1 idle2=0` | the precondition evaporated — the FAIL is the sampling artefact §3 describes |
| `idle=1 idle2=1` | an AP was idle at both instants and no IPI was delivered — the kick really was owed and really was missing, a scheduler defect |

## 6. Mutation check

The FAIL branch cannot be reached locally, so it was forced: `else if (0 && …)`
on the OK branch, kernel `234adcfec677a702`. It printed

```
[smp] resched FAIL ipis=1 ran=1 idle=1 idle2=1
```

so the field is wired and reachable. `ipis=1` there is the mutant's doing — the
kick *was* delivered on that run and the verdict was forced anyway — and
`idle2=1` on a healthy boot is the expected reading, which is what makes
`idle2=0` in a real failure informative.

Clean kernel `55446cb042530e80`, `smoke-rqstress-liveness` 2/2 PASS,
`hygiene_check.sh` all three PASSED.

## 7. Not claimed

This closes nothing. It does not fix the flake, does not bound its rate (one CI
occurrence, no local reproduction in 5 attempts), and does not decide between the
two readings in §5 — it makes the next occurrence decide. Until then
`resched FAIL` remains able to redden any gate it lands in.
