# HANDOFF — resume anchor for a fresh session

Updated after every merge-ready PR, per operator comment 5976228741 (PR #27,
2026-10-04). A fresh session should be able to resume from this file alone.

## Governing rule (operator, 2026-10-04)

**One issue → one branch off `dev/phase1` → one DDR → one gate with a mutation
check → one PR into `dev/phase1`.** Do NOT stack unrelated work on
`dev/phase1-seyp3n`. Keep PRs small. Work Steps 0–3 in order, autonomously; stop
and ask ONLY for an operator decision (a `[QUESTION]` in a plan file, a
trade-off, or anything on the not-approved list below).

## Not approved (do NOT do without the operator)

- Any Dependabot dependency bump (PRs #8, #9, #19, #30).
- The three PROPOSED plan items (ledger-seed wrap; rollback-undetected-in-v1;
  no-KCSAN-before-v1).
- Any claim that recent fixes close U1, U2, U3, OPEN-1 or OPEN-2.
- Merging to `main`, tagging `v1.0.0`, or promoting anything.

## Current kernel

- `kernel.bin` = `f5124d9b145df8cb`, 1,474,954 B, warning-clean at `-Werror`.
- GLOBAL_FORBIDDEN = 77 ; gates = 187 across 10 shards, 6 excluded.
- DDR free range: **DDR-1158+** (1156 = Fix 1, 1157 = Fix 2 both consumed).

## Branches / PRs in flight

- **PR #29** (`dev/phase1-seyp3n` → `dev/phase1`, DRAFT): the pre-rule stack —
  DDR-1154 (`3d2e2cf`) → DDR-1155 (`4e8f231`) → T1 plan (`f43ccbc`) →
  Fix 1 DDR-1156 (`9c917b0`) → Fix 2 DDR-1157 (`a0dda18`). Verified single linear
  history, no rewrite. Title/body updated 2026-10-04 to list all four.
  CI on tip `a0dda18`: `pradyos-ci` push + pull_request green; third
  (`workflow_dispatch`, run 37174876900) dispatched to meet the 3-green rule.

## Done

- **Fix 1 — DDR-1156** (virtio-blk slot-wait timeout UAF): unlink the giving-up
  thread under `compl_lock`; `THREAD_BLOCKED` guard in `slot_wake_one()`. Gate
  `smoke-blkslot`, M1 two-sided. Committed `9c917b0`, on PR #29.
- **Fix 2 — DDR-1157** (checkpoint/resume raw-pointer UAF): find + act inside one
  `g_sched_lock` critical section (`sched_checkpoint_pid`/`sched_resume_pid`).
  Gate `smoke-checkpoint`; M2 (drop `-ESRCH`) catches; M1 (resume order) is an
  **equivalent** mutant under the lock (recorded in DDR-1157 §5). Committed
  `a0dda18`, on PR #29. Regression: smoke-shell 5/5 + 3 block/liveness gates +
  3 static checks all green.
- **Step 0.1** (housekeeping): verified DDR-1154 (`3d2e2cf`) and DDR-1155
  (`4e8f231`) ARE on `dev/phase1-seyp3n` (ancestors of the tip). The earlier
  "dropped/reset" note was a stale-local-ref error from out-of-order
  check-suite webhooks — corrected. No history rewritten.
- **Step 0.2**: PR #29 title + body updated to describe all four items.
- **Step 0.3**: third CI run dispatched on `a0dda18` (run 37174876900).

## Hunt status (Step 1)

- **DDR-1154** merged-tree OPEN2_HUNT: pinned `182c30bb16930d57`,
  `HUNT_HB_FLOOR=17000`. Running total **5,445 read boots, 0 signals, 0 silent
  stops**, 95% upper bound ≈0.055%/boot. Dispatch 6 = run 37133287464 — **finish
  and record it** (Step 1). Do NOT pool with DDR-1139 §8 (different binary).
- **Next (Step 1):** a shorter confirmation hunt on the final tip
  (`f5124d9b145df8cb` or newer) + an installed-disk hunt exercising
  `virtio_blk.c`. Report signals / silent stops / confidence bound. Do NOT pool
  with the old binary.

## Next, in order

- **Step 1** — finish + record DDR-1154 dispatch 6; confirmation hunt on the new
  kernel; installed-disk hunt over `virtio_blk.c`.
- **Step 2** — lifetime audit (8 bug classes, comment 5971011693, read-only) →
  `docs/AUDIT_LIFETIME_2026-10.md`; one PROPOSED PR per worth-fixing finding;
  finish the PENDING KASAN regression test as its own PR with a before/after
  mutant.
- **Step 3** — T1 hardware/components per `docs/T1_ISO_PLAN.md` (L1–L7) and
  `docs/T1_REQUIREMENTS_REGISTER.md`, dependency order, one PR per component:
  USB first (xHCI → hubs → HID kbd/mouse → mass storage → USB-C), then Ethernet,
  audio, HDMI/DP + multi-display, Thunderbolt, then touch (emulated only) and the
  rest of the 22-item register. Native GPU drivers stay post-release. For each:
  DDR first, driver/support, a QEMU/VMware gate with a mutation check, and state
  plainly what is verified (emulated) vs not (real hardware).
