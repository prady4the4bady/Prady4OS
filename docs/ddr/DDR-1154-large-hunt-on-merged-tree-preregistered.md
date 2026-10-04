# DDR-1154 — The large OPEN2_HUNT campaign on the merged tree, registered before the data

**Status:** PRE-REGISTRATION. It is committed before the first dispatch. No kernel change, no gate, no new sentinel. `kernel.bin` is not rebuilt in the shipping tree.
**Asked for by:** operator decision on PR #17, comment 5861440399 (author_association OWNER, checked on the PR itself).
- The merge is authorised.
- `v1.0.0` stays untagged until one of two things holds:
  - **(A)** a substantially larger OPEN2_HUNT campaign completes clean; or
  - **(B)** each of the three unexplained signatures (DDR-1151 U1/U2/U3) has a named mechanism or an explicit "cannot close with current evidence" write-up.
- This DDR covers (A). DDR-1155 covers (B).

## 1. What "substantially larger" can mean on this harness

The harness puts a ceiling on each dispatch:
- `open2-hunt.yml` rejects `lanes > 20`.
- Each lane runs on one runner with one QEMU (§NON-NEGOTIABLE 12).
- DDR-1136 measured that `runs > 55` overruns the job's 180-minute timeout. The runs that finish are then the ones with short signal runs, which is a selection effect.

So one dispatch is at most **1,100 boots**. That is exactly the size of each of DDR-1139 §8's two confirmation dispatches. A single larger dispatch is not available.

Size therefore has to come from **several dispatches on one pinned binary**, pooled into one denominator. That pooling is only honest if every dispatch runs the same binary, and that is checked, not assumed (§3).

**The registered size is ten dispatches of 20 lanes × 55 runs: 11,000 boots.** That is five times the 2,200 boots of the confirmation, on a different binary: the merged tree, which carries DDR-1140..1153.

## 2. Why the old confirmation was blind to OPEN-1 route 1, and the check that fixes it

The campaign only counts a run as a signal if a SIGNALS pattern appears: `[schedcheck]`, `[apfreeze]`, `panic_stage=`, a panic banner, `gs FAIL` or `[ringwalk]`.

**OPEN-1 route 1 is a whole-machine silent stop.** DDR-1151 §6 reproduced that shape in 4 of 30 forced boots: no panic, no `[apfreeze]`, no heartbeat. On the campaign:
- If the stop comes **after** `rqstress_proof`, the run prints `churn clean`, which is indistinguishable from a healthy boot.
- If it comes **before**, the run is `NO-CHURN`, and the lane only fails when **every** run is `NO-CHURN`.

So the 2,200-boot confirmation bounded OPEN-2's signatures and said nothing about route 1. That is not a defect in DDR-1139 §8, which claimed only the former. It is a gap this campaign closes.

**The information is already in the job log.** Every per-run line prints the last heartbeat, `[hb] t=N`. Measured on run 36019681198, lane 9: all 55 runs end at **`t=17500`**, because the 180-second window at 100 Hz lands on that heartbeat.

A machine that stopped silently stops heartbeating, so its last `t` is lower. **The route-1 check is registered as: any run whose last heartbeat is below `t=17000`, carries no SIGNALS match, and is not `VACUOUS-CAPTURE`.**
- The 500-tick margin is one heartbeat interval, so a boot that runs slightly slow does not trip it.
- It needs no code change and no harness change. It is applied by reading every per-run line of every lane, not only the lanes that failed.

## 3. Pinning the binary

- Every dispatch passes `ref` as the **full 40-character SHA** `49abebc5f3ba0412bb7ff56f4c2a520a5014cb97`. That is `dev/phase1-seyp3n` at the moment this DDR's parent was written: the merge commit `772ab25` plus one `SESSION_HANDOFF.md` commit, so no build input changed.
  - DDR-1132 found that an abbreviated SHA voids every lane.
  - DDR-1138 measured that the 40-character form works.
- Every lane prints `kernel_pinned=`. **Pooling requires every lane of every dispatch to print the same value.** Any lane that differs is reported on its own and left out of the pool.
- This DDR's own commit is docs-only. It is **not** the pinned ref; the ref stays the commit above.

## 4. Pre-registered readings

| observation | reading |
|---|---|
| Every lane of a dispatch fails | **setup failure** (DDR-1132): the dispatch contributes **zero** boots and is re-dispatched. It is never read as a find. |
| `[schedcheck]` / `[apfreeze]` on this binary | **OPEN-2 recurs post-fix.** Resolve the RIP against *this* binary (§INV.18) before anything else. At `sched_exit+0x1b0` it means a duplicate-token source DDR-1139 did not find (DDR-1151 §4). Stop dispatching and investigate. |
| `loser_vec=13` at `resolve+0x61` (U1's shape) | **U1 recurs on a post-fix binary.** This **refutes** attributing U1 to the DDR-1139 mechanism (DDR-1151 §7). New investigation. |
| A panic whose body reaches the log (DDR-1135 printer) | Read the body. Its RIP decides its class. U2's own body stays lost. |
| `[vblkto]` (DDR-1148) | U3's instrument fired. Read `type`/`status`/`used_idx`/`late`. |
| A route-1 candidate (§2) | **OPEN-1 route 1 recurs post-fix.** This refutes the DDR-1151 §6 consistency reading. Pull that lane's artifact capture if the proxy allows; otherwise report the blocked host. |
| `churn_runs < runs` in a lane | Read that lane's `NO-CHURN` runs. If the heartbeat is below the threshold, the route-1 row applies. At full `t` with no churn, DDR-1097 §7.2's starvation applies: **not a signal, and not counted** in the denominator. |
| Nothing above, over the full campaign | **Criterion (A) met.** |

**What a clean result allows us to say, stated before the result is known.**
- The denominator is **Σ `churn_runs`**, read from every lane's own `DONE` line, not the dispatch parameters. That is DDR-1134's discipline, and here it applies to every lane, not just the failed ones.
- At 11,000 boots with 0 signals, the exact 95% upper bound is **≈0.027% per boot on this binary**, against the pre-fix binary's ≈0.36% (DDR-1139 §8). P(0 in 11,000 | 0.36%) ≈ 6e-18.
- For the route-1 check: route 1's historical rate has **no denominator** (DDR-1011 §5, DDR-1124). So a clean result bounds it only under this workload at `-smp 4`, and says nothing about CI's `smoke-surfdestroy`.
- **Not pooled** with DDR-1139 §8's 2,200 boots, which ran a different binary.

**What a clean result does NOT allow:**
- It names no mechanism for U1, U2 or U3. It makes their recurrence unlikely **under this workload**, and nothing more.
- It does not close OPEN-1 route 1 on CI gates. Route 1's recorded occurrences were on `smoke-surfdestroy` and `smoke-msixap`, not on this boot.
- It is not a tagging decision. The tag is the operator's call.

## 5. Stopping rule

- Dispatches run **two at a time**, so that queued jobs do not age out.
- **Stop at the first signal-bearing dispatch.** Resolve it before spending more boots: a finding changes what the remaining dispatches are for.
- Otherwise, stop after ten completed non-setup-failure dispatches.
- Results are recorded in §6 as they arrive, **appended rather than rewritten**.

## 6. Results

(appended as dispatches complete)

### 6.0 Amendment, written before any dispatch had reported: the route-1 check moves into the harness

- **Dispatches 1 and 2** (queued 2026-09-28 ~01:45Z on ref `49abebc`) run the **old** campaign. Their route-1 check is done **by hand**, exactly as registered in §2: every per-run line of every lane is read for its last `[hb] t`.
- **Dispatches 3–10** run a campaign that makes the check itself:
  - `hunt_silent_stop` (`tools/ci/hunt_print.sh`) flags a run with **zero SIGNALS matches** whose last heartbeat is below `HUNT_HB_FLOOR=17000`.
  - The campaign counts that run in `signal_runs`, which fails the lane under the default-branch workflow's existing `signal_runs=[1-9]` check. It also counts it separately as `silent_runs=`, and prints the last 40 lines of the capture, because for a silent stop the evidence is where the output **ended**.
  - Reading 200 lanes by hand (~500k tokens) and relying on a person to do it every time was the alternative. It is not a check.
- **Proved in both directions:**
  - `ci-huntprint-selftest` gains three fixtures: healthy to 17500, stopped at 4500, and stopped at 9000 **after** `rqstress OK`. The last is the case the old campaign printed as `churn clean`.
  - M4 (floor defeated) fails the two stopped arms and nothing else.
  - M5 (first heartbeat instead of last) fails the healthy arm and nothing else.
  - End to end on this host, one real run: a healthy boot reads `t=17500 churn clean`, `silent_runs=0`. The same run with `HUNT_HB_FLOOR=20000` reads `*** SILENT-STOP ***`, `signal_runs=1 silent_runs=1`, so the wiring into the lane verdict is exercised, not just the function.
- **The pooling rule is unchanged.** The diff touches only `tools/ci/`, not the kernel. `kernel_pinned` must still be identical in every lane of every dispatch, and any lane that differs is reported on its own.

### 6.1 Dispatches 1 and 2 (old campaign, ref `49abebc`): clean, 2,200 boots

**Runs.** `open2-hunt` 36366830314 and 36366831871, both `conclusion=success`, lanes=20 runs=55 hunt=32, `ref_requested` = `tree_sha` = `49abebc5f3ba0412bb7ff56f4c2a520a5014cb97` on every lane read.

**How it was read.** All 40 job logs were read, every lane, not just failed ones (§4's denominator rule). The old campaign does not detect route 1, so that check was done by hand per §6.0: every one of the 2,200 per-run lines was read for its last `[hb] t`.

**Results, by §4 row:**
- **Setup failure:** none. 0 of 40 lanes failed.
- **Pooling:** every `DONE` line reads `kernel_pinned=182c30bb16930d57`. All 40 lanes pool.
- **Denominator:** every `DONE` line reads `runs=55 signal_runs=0 churn_runs=55`, so Σ `churn_runs` = **2,200**. No NO-CHURN run means the starvation row never applied.
- **`[schedcheck]` / `[apfreeze]` / U1 / panic body / `[vblkto]`:** none. Every per-run line reads `rc=0 … churn clean`.
- **Route-1 candidate (last heartbeat below `t=17000`):** **none.** Final heartbeats fall in {17000, 17500, 18000}. Exactly **one** run sits at the floor: dispatch 2, lane 18, run 52, `t=17000 churn clean`. Under the registered rule (*below* 17000) it is not a candidate. It is recorded anyway, for the reason below.

**A calibration datum, not a finding.** `HUNT_HB_FLOOR=17000` (§6.0) sits at the **bottom edge of the healthy spread**, not below it. In 2,200 healthy runs, one finished exactly on the floor, so a healthy boot landing at 16500 is plausible.
- If that happens, it shows up as a `SILENT-STOP` **with churn and no SIGNALS**.
- That case is read off the last 40 capture lines, which the new campaign prints.
- A heartbeat that is merely late, with `rqstress OK` and the boot still progressing, is distinguishable there from a machine that stopped.
- The floor is **not** moved mid-campaign. §2 and §6.0 fixed it before any data arrived, and moving it now, in either direction, would make the remaining eight dispatches a different experiment. This is stated so that a borderline flag is read rather than reflexively counted.

**Running total for criterion (A):** 2,200 of 11,000 boots on `182c30bb16930d57`, 0 signals. The interim 95% upper bound is ≈0.136% per boot. Per §4 that is **not pooled** with DDR-1139 §8's 2,200, which ran a different binary.

**Dispatches 3 and 4** were sent with ref `473b67536ceb94b440492ae6b172f853023a93a4`, the **new** campaign with the silent-stop check, as runs **37097190752** and **37097192020**.
- `473b675`'s diff from `49abebc` touches only `tools/ci/` and docs, so every lane must still print `kernel_pinned=182c30bb16930d57` (§6.0).
- A lane that prints anything else is reported on its own.

### 6.2 Dispatch 3 (new campaign, ref `473b675`): clean, 1,100 boots; dispatch 4 still running

**Dispatch 3, run 37097190752: all 21 jobs succeeded.** The 20 lanes plus setup are 0 of 20 failed, so the setup-failure row does not apply.
- Every lane's `DONE` line was read: `runs=55 signal_runs=0 silent_runs=0 churn_runs=55 kernel_pinned=182c30bb16930d57`.
- §6.0's prediction held on every lane: the `tools/ci`-only diff produced the same pinned binary, so the dispatch pools with 1 and 2.
- **`silent_runs=0` on all 20 lanes.** This is the first dispatch where route 1 is checked by the harness itself (`HUNT_HB_FLOOR=17000`) rather than by hand, and it found nothing.
- No `SILENT-STOP` tail was printed, so there is nothing to read.

**Dispatch 4, run 37097192020,** was still running when this was written and will be recorded in §6.3. **Dispatch 5** went out on the same ref and parameters as run **37108041701**, keeping two in flight per §2. Dispatch 6 goes out when dispatch 4 completes.

**Running total for criterion (A):** **3,300 of 11,000 boots** on `182c30bb16930d57`, 0 signals, 0 silent stops. The interim 95% upper bound is 3/3300 ≈ **0.091% per boot**. It is still not pooled with DDR-1139 §8.

### 6.3 Dispatch 4 (new campaign, ref `473b675`): clean, 1,100 boots. Dispatch 6 held because CI is starved

**Dispatch 4, run 37097192020: all 21 jobs succeeded.** That is 0 of 20 lanes failed, so the setup-failure row does not apply.
- All 20 lanes' `DONE` lines were read, not just the job conclusions, per §4's denominator rule. Every one reads `runs=55 signal_runs=0 silent_runs=0 churn_runs=55 kernel_pinned=182c30bb16930d57`.
- Lane 14's per-run tail was read in full: runs 45–55 all read `rc=0 [hb] t=17500 churn clean`.
- Lanes 3 and 4 ended with a final heartbeat of `t=18000` and the rest at `t=17500`. All of these are above the floor.

**Running total for criterion (A):** **4,400 of 11,000 boots** on `182c30bb16930d57`, with 0 signals and 0 silent stops. The interim 95% upper bound is 3/4400 ≈ **0.068% per boot**. It is still not pooled with DDR-1139 §8.

**Dispatch 6 is HELD, by operator instruction** ("hold new dispatches if they starve pradyos-ci"). The starvation was measured at the time of writing:
- `pradyos-ci` push runs on `77a96da` (37097437825, queued since 04:42Z) and `db7d3c1` (37108061827) are still **queued, not started**.
- Further `pradyos-ci` runs on `dev/phase1` (`9df3c17`, `ee7f0b6`) and on PR #27 (`68badec`, `3aa8a40`) are queued behind them.
- **Dispatch 5 (37108041701) is itself still queued.**

The two-in-flight rule of §2 would now be a twenty-runner claim competing with every CI suite. The rule is therefore relaxed to *"one in flight while CI is queued"*. Dispatch 6 goes out only once those `pradyos-ci` runs have started. This changes the **pace** of the campaign, not its design: the criterion, the binary, the floor and the 11,000 target are all unchanged.

### 6.4 Dispatch 5 (ref `473b675`): 19 lanes clean, lane 9 UNREAD after a job timeout. Starvation cleared; dispatch 6 sent

**Dispatch 5, run 37108041701, concluded `cancelled` overall, and the ONLY cause is lane 9.** That lane (job 111180660197) hit the workflow's 180-minute job limit. Its annotation reads *"The job has exceeded the maximum execution time of 3h0m0s"*. The job started 10:05:39Z and was cancelled at 13:10:39Z, with the Hunt step still in progress.
- **Lane 9 is UNREAD.** The log download returns 404 and the upload step never ran, so no artifact exists.
- It is therefore **excluded from the denominator.** It is **not counted clean**, because §4's rule is that a boot counts only if its `DONE` line was read.
- It is **not counted as a signal** either. A cancelled job is not the inverted-polarity `failure` the workflow uses to report a find.
- **The cause is not established.** It could be a slow runner. It could be a run that never terminated: a boot that hangs past `boot_test.sh`'s own `timeout`, or the harness itself. Nothing in hand distinguishes the two.
- The other 19 lanes took about 2 h 46 m for 55 boots, so the margin to the 3 h limit is about 14 minutes. A runner some 8% slower would hit the limit with no defect anywhere. That is a reason to suspect a slow runner, **not evidence of one**, and nothing is concluded from it.
- **What would settle it is the per-run capture, and none exists.** If a later lane times out, the remedy is to shrink `runs` so the margin grows (for example 50 boots at about 2 h 31 m). It is **not** to raise `timeout-minutes`, which would leave the next unreadable lane equally unreadable. That would be a pace change, not a design change; it is recorded here and not made yet.

**The other 19 lanes (0–8 and 10–19) succeeded, and every `DONE` line was read.** Each reads `runs=55 signal_runs=0 silent_runs=0 churn_runs=55 kernel_pinned=182c30bb16930d57`. Lane 14 finished at 14:17Z, later than the rest, and still reads clean.

**Running total for criterion (A):** 4,400 + 19 × 55 = **5,445 of 11,000 boots** on `182c30bb16930d57`. That is 0 signals and 0 silent stops, with 55 boots unread and excluded. The interim 95% upper bound is 3/5445 ≈ **0.055% per boot**. It is still not pooled with DDR-1139 §8. To reach 11,000 the campaign needs 5,555 more read boots, which is about 5.05 further dispatches at 1,100 each. The target is a count of **read** boots, so lane 9's loss is made up rather than written off.

**The starvation has cleared.** At the time of writing no `pradyos-ci` or `open2-hunt` run is non-completed. The `77a96da` (37097437825) and `db7d3c1` (37108061827) suites both completed successfully, and PR #29's head `d667e8a` has both suites green. §6.3's hold is therefore lifted.
- **Dispatch 6** goes out on the same ref and parameters: `ref=473b67536ceb94b440492ae6b172f853023a93a4`, `lanes=20`, `runs=55`, `hunt=32`.
- The rule stays *"one in flight while CI is queued"*. With nothing queued, a second dispatch may follow, but only after checking the queue again.

### 6.5 Dispatch 6 (ref `473b675`): run 37133287464 concluded `success` (clean), lane DONE lines UNREAD from this container

**Run 37133287464, `open2-hunt`, `workflow_dispatch`, concluded `success`** —
20 of 20 `hunt` lanes plus `setup` all `success` (verified via the jobs API:
every `hunt (lane 0..19)` conclusion is `success`). On this workflow's inverted
polarity (`signal_runs=[1-9]` fails the lane; `HUNT_HB_FLOOR=17000` wires the
silent-stop check into the lane verdict), an all-`success` run means **0 signals
and 0 silent stops** reached a failing verdict on any lane.

**Honesty about the denominator (§4's rule, and the limit this session hit).**
The per-lane `[campaign] DONE runs=55 signal_runs=0 silent_runs=0 churn_runs=55`
lines could **not be read from this container** — the job-logs endpoint
302-redirects to `productionresultssa*.blob.core.windows.net`, which this
environment's egress proxy refuses with `connect_rejected` (the blocker
`/root/.ccr/README.md` says to report, not route around). So for dispatch 6 the
boot count rests on the **dispatch parameters** (`lanes=20 runs=55` = 1,100
boots) together with the 20/20 `success` conclusion, exactly the footing
DDR-1133 §7 named ("'1000 boots' rests on the dispatch parameters"). It is
recorded as **conclusion-verified, DONE-lines-unread** rather than claimed read.
A session that can reach the log blobs (or the operator) should read the 20 DONE
lines and promote these 1,100 to "read".

**Running total for criterion (A):** 5,445 **read** + 1,100
**conclusion-verified (DONE-lines-unread)** = **6,545 of 11,000** on
`182c30bb16930d57`, 0 signals, 0 silent stops. Interim 95% upper bound on the
6,545 is 3/6545 ≈ **0.046% per boot**. Still **not pooled** with DDR-1139 §8 (a
different binary), and still not pooled with any new-tip hunt (§6.6), which is a
different binary again.

### 6.6 Step 1 confirmation hunt + installed-disk hunt (operator 5976228741) — planned, held on the starvation rule

The operator's Step 1 asks for two further hunts on the NEW kernel (the Fix 1 +
Fix 2 tree, `kernel.bin` `f5124d9b145df8cb`; the OPEN2_HUNT build of that tree
is a different binary again). **Neither is pooled with `182c30bb16930d57` (§6.5)
nor with DDR-1139 §8.**

1. **Confirmation hunt (new tip).** A plain `open2-hunt` dispatch,
   `ref=dev/phase1-seyp3n` (tip `a62e06a`), shorter than the campaign. **HELD at
   the moment of writing:** the three `pradyos-ci` suites on `a62e06a` (push +
   pull_request + workflow_dispatch — the Step 0.3 three-green set) are **queued,
   not started**. DDR §6.3's relaxed rule ("one hunt in flight while CI is
   queued") and the operator's standing "hold dispatches that starve pradyos-ci"
   both apply: a 20-runner hunt now would delay the three greens. It goes out
   once those suites start/clear, and the queue is re-checked immediately before.

2. **Installed-disk hunt over `virtio_blk.c`.** This is **not** an `open2-hunt`
   input — the campaign loop (`open2_hunt_campaign.sh`) boots `build/pradyos.img`
   via `boot_test.sh`, while the installed-disk boot path is a separate harness
   (`tools/qemu_runner/install_test.sh`, DDR-1143). Booting the installed disk in
   a churn loop that exercises the virtio-blk slot-wait path (Fix 1's subject) is
   a **new harness mode**, so under the operator's one-issue/one-branch/one-PR
   rule it is **its own issue**: own DDR, own campaign variant, own gate with a
   mutation check, own PR. Recorded here and tracked in `docs/HANDOFF.md`; begun
   as its own branch, not bolted onto this campaign.

**Reading limit, stated plainly:** this container cannot read hunt lane logs
(the job-logs endpoint 302s to a proxy-blocked blob host), so any hunt dispatched
from here yields only the run `conclusion` to this session. The lane `DONE` lines
must be read by a session that can reach the blobs, or by the operator. This does
not change what gets dispatched; it changes who can promote a run from
"conclusion-verified" to "read".
