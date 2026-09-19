# DDR-1082 — THE GROUP A `smoke-rqstress` 20× IS COSTED, AND IT IS ALREADY ON
# RECORD AS NON-DISCRIMINATING FOR THE CLASS IT WOULD BE RUN FOR

Status: RECORDED. **The 20× campaign is NOT run.** One timed run was, as the
cost measurement. No code change, no gate change, no kernel change; `kernel.bin`
`e638b8a7ee263944`, 1,290,634 B, unchanged; 178 gates unchanged;
`GLOBAL_FORBIDDEN` 76 unchanged; no open issue moves.

This is DDR-1061's shape — a campaign costed **before** the hours are spent
rather than after — and it reaches a stronger conclusion than DDR-1061 could,
because here the experiment has a recorded historical result.

---

## 1. WHAT THE ROW ASKS

CLAUDE.md Group A: *"`smoke-rqstress` determinism | 20× green before moving on |
`smoke-rqstress` 20×"*, under §NON-NEGOTIABLE 2's 20× rule for SMP / timing /
scheduler / intermittent gates.

## 2. THE COST, MEASURED NOT DERIVED

The gate declares `FORBIDDEN_SENTINEL="rqstress FAIL"`, so per DDR-1043 it is
never early-exit eligible and runs its full window by design. That is a rule;
this is the measurement:

    make smoke-rqstress   ->   rc=0, elapsed 181 s
    kernel.bin e638b8a7ee263944 before AND after (DDR-1060 §9's pin)

**20 × 181 s = 3,620 s = 60.3 minutes of foreground QEMU.** Foreground is the
only option — DDR-1060 §10 measured three background campaigns each killed after
1–5 runs when the session went idle, `setsid` included, because this container
executes only while a turn is live.

## 3. WHAT 20× WOULD BUY

With 0 failures in n runs the 95% upper bound is `1 − 0.05^(1/n)`:

| n | 95% upper bound |
|---|---|
| 20 | **13.9%** |
| 30 | 9.5% |
| 42 | 6.9% |

So an hour buys a **13.9% single-binary bound**.

## 4. WHAT IS ALREADY ON RECORD — AND WHY IT IS NOT SIMPLY BETTER

`smoke-rqstress` is registered on **shard 8, strict** (`gate_shards.txt:173`)
and is **not** in `shard_check.sh:50`'s exclude set, so it runs on *every* CI
suite. DDR-1062 audited a 42-suite window and attributed all four reds in it —
`smoke-actiondel` ×2, `smoke-nethammer`, `smoke-surfclose` (its own §table,
lines 53–55). **None is `smoke-rqstress`**, so that window carries ≥42 green
observations of this gate, i.e. <6.9% at 95%.

**That is a different claim, not a strictly better one**, and DDR-1062 says so
in its own §NOT CLAIMED: *"These are 42 suite runs across 19 SHAs, not 42
independent binaries."* The 20× rule exists to catch **intermittency on one
binary**; a multi-binary window does not substitute for it. Recorded as a
limitation rather than argued away.

## 5. THE DECIDING FACT: THIS EXACT 20× HAS BEEN RUN, AND IT PASSED THROUGH A LIVE DEFECT

`docs/BUILD_TRACKER.md:679`, corroborated verbatim in `SESSION_HANDOFF.md:6716`
and `docs/build_status.md:7286`:

> Gate lesson worth keeping: `smoke-smp` and `smoke-rqstress` both measured
> **20/20** at `-smp 4` **while this defect was live**. The gates did not catch
> it; the evidence sat in serial logs nobody asserted on.

The defect was DDR-981's — `yield()` spinning with `RFLAGS.IF` clear, a real AP
freeze, exactly the SMP/scheduler failure class this row's 20× exists to detect.
**A green 20/20 on this gate is therefore on record as not discriminating the
class it would be run for.** An hour spent reproducing that result reproduces a
measurement already known to be silent.

**And the replacement already shipped.** DDR-981 put `[apfreeze]` into
`GLOBAL_FORBIDDEN` *because* the 20× was silent — a **detector**, which reddens
whichever gate the freeze lands in, rather than a repetition count. `SESSION_HANDOFF`
records the reason it was chosen over ~20 recipe edits: it preserves every gate's
DDR-785 early-exit eligibility.

**A provenance note, checked rather than assumed** (DDR-1007's discipline):
`grep -niE 'rqstress' docs/ddr/DDR-981-*.md` returns **nothing**. The DDR's own
§5 records *its* campaign — "20 boots at `-smp 4`", the fix's evidence — and the
*gate lesson* was recorded beside it in the three trackers, not inside it. Not a
contradiction, and not a defect; worth knowing before someone greps the DDR
expecting to find the sentence quoted above.

## 6. VERDICT

**Do not run the 20× campaign on this gate.** It costs a measured 60.3 minutes
of foreground QEMU, buys a 13.9% single-binary bound, and repeats an experiment
whose recorded outcome on this exact gate is a clean 20/20 obtained while a real
AP freeze was live in the kernel.

The row's coverage today rests on two things, and the row should say so:

1. **Per-suite CI observations** — every suite, shard 8, strict; ≥42 green in
   DDR-1062's audited window, across 19 SHAs.
2. **`[apfreeze]` in `GLOBAL_FORBIDDEN`** — the detector that exists precisely
   because the repetition count was silent.

Neither is a single-binary determinism proof, and this DDR does not pretend
otherwise. What it establishes is that the *stated remedy* would not supply one
either.

## 7. WHAT WAS ACTUALLY MEASURED HERE

One run, on today's tip: **rc=0, 181 s, kernel `e638b8a7ee263944`**, hash
verified before and after. Reported so its absence is not mistaken for an untried
experiment — **not** as support for the verdict, because 1/1 bounds the rate
below 95% and that is all it does.

## 8. NOT CLAIMED

* **`smoke-rqstress` is NOT claimed deterministic**, and the Group A row is
  **re-stated, not closed**. One local observation is one observation.
* **No defect is named or fixed**, and none is alleged — the gate is green
  everywhere it has been looked at.
* **The 42-suite window is not offered as a substitute** for a single-binary
  20×; §4 states why it cannot be.
* **DDR-981 is not accused of anything.** Its fix, its own 20-boot campaign and
  its choice of `[apfreeze]` all stand; §5 quotes its lesson to *support* the
  verdict, not to revisit it.
* **No rate is inferred** for this gate beyond the two bounds stated, each
  labelled with what it covers.
* No code change; `kernel.bin` untouched, so the size/headroom pair and
  `ci-docstate-check` are unaffected. 178 gates unchanged, no new gate,
  `GLOBAL_FORBIDDEN` 76 unchanged. No open issue moves (OPEN-1/2/12/13
  untouched). No release action taken or proposed.
