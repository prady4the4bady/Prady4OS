# DDR-1127 — THE FIRST OPEN-2 HUNT DATASET: 60 BOOTS, ZERO SIGNALS, AND DDR-1097'S OWN PREMISE IS NOT MET

**Status:** measurement. **Docs-only** — no code change, no gate, `kernel.bin`
NOT rebuilt. **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE**
(NON-NEGOTIABLE 3). No open issue moves; OPEN-1/2/12/13 untouched.

---

## 0. What this is, and what it is not

DDR-1097 built the isolated CI hunting job. DDR-1117 then found it had **never
run and could not be started** — a `schedule`/`workflow_dispatch` workflow that
lives anywhere but the default branch is unstartable by any means — and PR #20
landed the file there. PR #21 then made it *runnable* (the harness lives only on
`dev/phase1-seyp3n`, so the file must be on one branch and the checkout on the
other).

**This is the first time the instrument has produced a dataset.** DDR-1097's own
NOT CLAIMED said *"a RECYCLED line has NEVER been observed outside its forced
build"*, and DDR-1117 explained that this could not have been otherwise. That
sentence is now backed by a number instead of by an absence.

**It is not a reproduction, and it is not a rate for OPEN-2.** §5 is the whole of
what it licenses.

---

## 1. The artefact

**Run `35501810702`**, workflow `open2-hunt`, event **`schedule`** — the weekly
cron, firing on its own for the first time. 2026-09-20 09:15:32Z → 09:46:42Z.
Six lanes, each a **separate GitHub runner with exactly one QEMU** (which is why
this is not operator approach #2 and NON-NEGOTIABLE 12 is not implicated —
DDR-1096 §2.2's refusal stands and this does not revive it).

Per-lane build line, read from the log:

```
[job] built with OPEN2_HUNT=32 lane=0 ref_requested=dev/phase1-seyp3n
      tree_sha=eba9cb34ada2c85413289d59dd2d10a11c5403e3
[campaign] kernel_pinned=ce42b14e72623f81 runs=10 smp=4
```

**`tree_sha=eba9cb3` is DDR-1125** — docs-only, i.e. the tree **before** the
CR0.WP fix. `ce42b14e72623f81` is the `OPEN2_HUNT=32` build of it, which differs
from the shipped `854bbb38fdfe4fd2` **because the flag rebuilds**, exactly as
DDR-1097 §6 requires; every lane's *"PROVE the flag rebuilt"* step passed, so the
build-side false-clean check (`A != B` on the **hash**, never the size) held six
times.

---

## 2. The numbers, READ FROM THE LOGS AND NOT INFERRED FROM THE CONCLUSION

This distinction is load-bearing here because **this workflow's polarity is
inverted** — its own header says *"IT FAILS WHEN IT FINDS SOMETHING … Do NOT
'fix' it to pass on a hit."* So `conclusion: success` **means clean**, and
reading the conclusion alone would be assuming the thing to be measured. All six
lane logs were opened:

| lane | runs | signal_runs | churn_runs | kernel_pinned |
|---|---|---|---|---|
| 0 | 10 | **0** | **10** | `ce42b14e72623f81` |
| 1 | 10 | **0** | **10** | `ce42b14e72623f81` |
| 2 | 10 | **0** | **10** | `ce42b14e72623f81` |
| 3 | 10 | **0** | **10** | `ce42b14e72623f81` |
| 4 | 10 | **0** | **10** | `ce42b14e72623f81` |
| 5 | 10 | **0** | **10** | `ce42b14e72623f81` |

**Totals: 60 boots, 60 with churn, 0 signals, ONE binary.**

Each run is a full default boot — `TIMEOUT_S=180 QEMU_SMP=4 KEEP_SERIAL=1` with
`FORBIDDEN_SENTINEL="__never_appears__"`, which per DDR-1043 disables the DDR-785
early exit, so **every boot burns its whole 180 s window by design** and reaches
`[hb] t=17500`. Not one run was `VACUOUS-CAPTURE`.

---

## 3. `churn_runs` is the load-bearing half, and it REFINES DDR-1097 §7.2

`signal_runs=0` on its own is worth nothing: DDR-1097 §7.2 found that the hunt
pauses slow the boot, so a run that never reaches `rqstress_proof` **never ran
the create/exit churn the race requires** — *"a wide window with nothing to catch
in it … it reports `clean` while testing nothing."* It measured **1 run in 3**
starving locally at this very working point, and built `churn_runs=` as the
denominator (NON-NEGOTIABLE 17) with a hard failure at `churn_runs=0`.

**On GitHub runners it is 60 of 60.** `CHURN` is `[smp] rqstress OK`, and
`rqstress_proof` spawns and exits a 24-thread burst — the unlink churn the
hypothesised race needs. So this is **not** a vacuous clean, and that is measured
rather than argued.

It also corrects the reach of DDR-1097 §7.2's own observation: **the 1-in-3
starvation was a property of that host, not of `OPEN2_HUNT=32`.** DDR-1097 was
careful to say *"one observation of a boundary is not the boundary"* and declined
to retune on three local boots. That caution was right, and 60 boots on different
hardware now say the working point is **better** on CI than the local figure
suggested. **`OPEN2_HUNT` is still not retuned here** — this is a reason not to
lower it, not a reason to raise it.

---

## 4. THE FINDING — DDR-1097's stated premise is not met

DDR-1097 §4 justified hunting the **precondition** rather than the freeze:

> *A RECYCLED line is not a wedge and not OPEN-2 reproducing — it is evidence the
> PRECONDITION occurred, which is the point: **the precondition is far commoner
> than the freeze, so a hunt that can see it gets a number in hours.***

**Sixty boots, every one with churn, produced ZERO.** The hunt got its hours and
did not get its number. That premise is **not met**, and it was the reasoning that
made this the instrument to build.

**THREE READINGS FIT, AND THIS DATASET CANNOT DISCRIMINATE THEM.** They are
recorded so the next session does not silently adopt one:

1. **The precondition is genuinely rare** — not "far commoner than the freeze"
   but comparable to it or rarer. DDR-1062 bounds the freeze at **<6.9% per
   suite**; §5's bound on the precondition is **<4.9% per boot**. Those are
   different units over different populations and **must not be compared
   directly**, but nothing here shows the precondition to be the abundant signal
   the design assumed.
2. **`OPEN2_HUNT=32` does not widen the window as much as assumed.** The pause
   demonstrably slows the boot (that is what §7.2's starvation was), but "slows
   the boot" and "widens *this* race" are different claims, and only the first
   has ever been measured.
3. **The detector is still blind in some way DDR-1097 did not close.** It closed
   the reuse blind spot DDR-1096 recorded against itself by keying on `tid` (one
   writer, `sched.c:1053`, from a monotonic static) — and the *forced* build
   (`OPEN2_FORCE_RECYCLE=1`) proves the **wiring**, which DDR-1097 said in as
   many words is *"a proof of WIRING and nothing else"*, because the inverted
   build fires on node 1 every tick so the DDR-955 sweep effectively does not
   run. **Wiring proven ≠ sensitivity proven**, and nothing has ever tested the
   latter.

**This is the first evidence of any kind bearing on DDR-1096 §3's hypothesis**
(the unlocked all-threads ring walk in `sched_tick`), which has been a hypothesis
with nothing under it since it was proposed. **It points away from it and does not
refute it** — reading 3 alone blocks a refutation, and the hunt boots are one
workload while OPEN-2 has appeared on many different gates (`smoke-smpuser`,
`smoke-smpsched`, `smoke-nethammer`, `smoke-blkmq-trace`, `smoke-smplock`,
`smoke-blk-integrity`). **A matching absence is not a refutation any more than a
matching shape is a mechanism (DDR-1056).**

---

## 5. What it bounds, and what it does NOT

0 failures in *n* gives a 95% upper bound of `1 − 0.05^(1/n)`:

* **n = 60 → the precondition fires on fewer than 4.87% of boots**, on this
  binary, under this workload, at `-smp 4`, with `OPEN2_HUNT=32`.
* `P(0 in 60 | p = 0.10) = 0.0018` and `P(0 in 60 | p = 0.25) = 3.2e-08`, so a
  precondition rate at either of those figures is strongly disfavoured.

**IT IS A RATE FOR THE PRECONDITION DETECTOR, NOT FOR OPEN-2.** The workflow's own
error text says it: *"A `[ringwalk] RECYCLED` line is EVIDENCE THE WINDOW OPENED,
not a reproduction of the freeze and not a mechanism."* Nothing here moves
DDR-1062's CI-side freeze bound.

**ONE BINARY.** DDR-1062's own NOT CLAIMED warns against pooling — *"42 suite runs
across 19 SHAs, not 42 independent binaries"* — and the same discipline applies in
reverse here: this is **60 boots of one binary**, which is the *stronger* shape for
a single-binary bound and says nothing about any other tree. In particular it says
**nothing about the CR0.WP kernel**, because it predates it.

---

## 6. A second run is in flight and MUST NOT BE POOLED WITH THIS ONE

Run `35504467004` was dispatched manually at 10:13Z with the same shape (6 × 10,
`OPEN2_HUNT=32`) against `dev/phase1-seyp3n` — which now carries **`a390eab`, the
CR0.WP kernel**. That is a **different binary**, so its result is a separate datum
and pooling the two would be exactly the error §5 guards against. It is also,
incidentally, the first hunt to run with kernel W^X actually enforced against
ring 0.

---

## 7. NOT CLAIMED

* **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE.** NON-NEGOTIABLE 3 holds:
  no artefact was captured, because the point of this DDR is that none was.
* **DDR-1096 §3's hypothesis is NOT refuted** — §4 reading 3 blocks that, and the
  coverage is one workload.
* **DDR-1097 IS NOT WITHDRAWN OR CRITICISED.** Its harness is correct, its `tid`
  keying is the right token, its build-side and churn guards both did their jobs
  here (six `A != B` proofs, 60 non-vacuous captures), and its refusal to retune
  on three local boots was the right call. What is recorded is that **one sentence
  of its rationale — the reason it was worth building — is not borne out by the
  first dataset it produced.** That is the instrument working: it was built to get
  a number, and the number is informative.
* **`OPEN2_HUNT` is NOT retuned**, on DDR-1097's own reasoning; §3 is a reason not
  to lower it and not an argument to raise it (DDR-1096 §4.2 measured 128/512
  starving the boot outright).
* **NO code change, NO gate, NO new sentinel**, `kernel.bin` NOT rebuilt — so the
  size/headroom pair and `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN`
  77, **179 gates**, 79 probe ELFs.
* **NO campaign was run locally** and none should be: DDR-1023 established the
  local route is exhausted (56 clean runs across the two kernels that mattered),
  and this is the CI-side route it named as the live one.
