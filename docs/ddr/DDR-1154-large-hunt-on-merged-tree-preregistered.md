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
