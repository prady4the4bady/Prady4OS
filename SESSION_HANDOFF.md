# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-19)

- **dev/phase1 tip:** `43d62c1` (#148, DDR-1208: virtio-blk bounce buffers and timed-out slot quarantine; mutants Q1, B1 red on `4298fde`). Before it: `3971031` (#147, DDR-1183 Amendment 1: stage-2 window 52 -> 53 chunks, gate 1,736,704 B), `92a89de` (#146, bookkeeping), `fa96706` (#145, C1.5 P5).
- **Kernel:** default `kernel.bin` 1,687,946 B (48,758 B headroom); the binding `RCU_TORTURE=1` build 1,704,330 B (32,374 B headroom), both printed by CI on `4298fde` (run 44540005938), against the 1,736,704 B gate. The PT_HI ceiling (image plus BSS at or below phys 0x600000) is the next wall: about 35 KB left, build-enforced (DDR-1183 A1). NSI: max 124 (`SYS_PSI`); next free 125. DDR numbers: next free is 1209. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Standing instruction: #45 6993979920 (Nov 19).** Build every remaining chunk in checklist order, one feature per PR: DDR, code, CI gate, mutants red, merge. Merge each PR yourself under the five conditions of 6594914166 (green on the exact head, `mergeable_state` clean, mutants red or recorded not gated, CodeRabbit answered, previous PR merged). Update the checklist and this file after each merge; post a short status on #45 after each chunk. At most 3 CI runs at a time.
  - **Still wait for the operator on:** drafts #68 and #85 (the operator merges them); any key, secret, credential or trust-root step (C3.3: write the DDR, open the PR, stop); C6.2 branding.
  - **Blocker rule:** a red or inconclusive gate you cannot explain, a recurrence of an unnamed failure, or a CodeRabbit finding that shows the design is wrong: one blocker comment, mark the chunk BLOCKED, take the next unblocked chunk.
- **Order now (the operator's, 6993979920):** C1.5 P5 (PSI), P6 (tickless), deadlock detection (C1.4), C1.6, C1.7, then Phase 2 onward by `Needs`. C11.1 stays last. This order puts deadlock detection after P5 and P6, although the checklist lists C1.4's row first. The operator chose that order (as 6761846242 did before it), and the newest owner comment wins over both the checklist order and DDR-1188's "No code until C1.4 has merged". P1–P4 were built under the same orders and run on C1.4's merged parts (threads, futex, `psync`, RCU).
- **Branches ready:**
  - `c15-p6` at `64fa91f`: DDR-1188 Amendment 7 only (design: APs only, a bounded 1 s stop, an RCU extended quiescent state, a probe-only isolate op for arms L, L1, L2). Read it before writing P6 code. The window is now 53 chunks (DDR-1183 A1); P6's growth must stay under both the size gate and the PT_HI ceiling.
- **#135 (open):** the `-ENOMEM` mechanism is named and fixed (#142). Arm R was green in 5 CI runs on trees with P4 and the fix; it failed 2 of 6 with P4 before it. What P4 changes to raise the rate is not explained. Close it only after repeated green runs; a recurrence is a blocker.
- **Issues open:** #135, #143 (arm C2 `net_leak_ok=0`, 1 in 202 runs, no mechanism), #116, #117, #119.
- **Review threads left open for the operator:** the Codex `docs/build_status.md` notes on #136, #138, #140, #142 and #148 (question in #45 6948599917).
- **Follow-ups recorded, not started:** the four other waits that ignore a handlerless SIGTERM (DDR-1206 §4); `signal_deliver` acting on default-terminate signals while a handler is active (DDR-1206 A1); the surface logging DDR-1199 proposes.
- **Lessons:**
  - Push work as soon as it builds. A container replacement lost an uncommitted build once.
  - Never `git checkout <file>` to drop a mutant from a tree with uncommitted work; revert the mutant with its own script.
  - A probe's design assumption about kernel state must be checked by running it before the gate is trusted.
  - A probe arm that a mutant can damage must restore what it snapshots whatever the call returned.
  - `hygiene_check.sh` runs `make` targets: never run it while a local gate loop is using the same build directory.
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A pull_request run that fails for a runner outage keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
  - The `github-advanced-security` check fails with "exceeded your monthly quota" (#134); not a required check.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- Merge this bookkeeping PR (docs-only rule).
- Then P6 from the Amendment 7 design (`c15-p6`, merge `dev/phase1` in first). Carry CodeRabbit's #145 note into P6's `rt_tick` comment: `rq_has_ready()` is a hint.
- DDR-1208 follow-up, not started: the virtqueue is not given back when a virtio-blk probe fails after queue setup (predates DDR-1208; noted on #148).

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
