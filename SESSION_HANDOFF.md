# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-20)

- **dev/phase1 tip:** `0d464e1` (#154, C1.6 P1 user ASLR, DDR-1189 Amendment 1; head `da6ce27`, run 44778658685; mutants D1, D2, D2b red on their arms). Before it: `3c4e36d` (#153, bookkeeping), `b8ab375` (#152, C1.4).
- **Kernel:** default `kernel.bin` 1,700,242 B (36,462 B headroom); the binding `RCU_TORTURE=1` and `LOCKDEP=1` builds 1,716,626 B (20,078 B headroom), all printed by CI on `da6ce27` (run 44778658685), against the 1,736,704 B gate. C1.6 P2 adds 4,096 B to the default build (local, 1,704,338 B). The PT_HI ceiling (image plus BSS at or below phys 0x600000) is build-enforced (DDR-1183 A1); a build's own margin is 0x600000 minus its `__bss_end` physical address (local, before P6: default 100,672 B, `LOCKDEP=1` 72,448 B). NSI: max 124 (`SYS_PSI`); next free 125. DDR numbers: next free is 1210 (C1.6 P1 and P2 are amendments, DDR-1189 A1 and A2). Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Standing instruction: #45 6993979920 (Nov 19).** Build every remaining chunk in checklist order, one feature per PR: DDR, code, CI gate, mutants red, merge. Merge each PR yourself under the five conditions of 6594914166 (green on the exact head, `mergeable_state` clean, mutants red or recorded not gated, CodeRabbit answered, previous PR merged). Update the checklist and this file after each merge; post a short status on #45 after each chunk. At most 3 CI runs at a time.
  - **Still wait for the operator on:** drafts #68 and #85 (the operator merges them); any key, secret, credential or trust-root step (C3.3: write the DDR, open the PR, stop); C6.2 branding.
  - **Blocker rule:** a red or inconclusive gate you cannot explain, a recurrence of an unnamed failure, or a CodeRabbit finding that shows the design is wrong: one blocker comment, mark the chunk BLOCKED, take the next unblocked chunk.
- **Order now (the operator's, 6993979920):** C1.5 P5 and P6 and deadlock detection (C1.4) are done; C1.6 is in progress (P1 done); then C1.7, then Phase 2 onward by `Needs`. C11.1 stays last. C1.5 stays IN PROGRESS only for memory PSI, which waits for C2.1.
- **Branch ready:** `c16-p2-pie` at `d430205` (pushed; PR not opened yet): C1.6 P2, static PIE (DDR-1189 Amendment 2), gate `smoke-dynlink` arm C as C1–C5 (shard 8).
  - Merge `dev/phase1` in first (#154 and this bookkeeping). The code on `d430205` already holds P1.
  - Locally on `d430205`: `smoke-dynlink` green (C1 `rel=4/4`, C2 `distinct=4 inrange=4`, C3 `aux=4/4`, C4 and C5 refused with -8); mutants D3, D3b, D3c, D3d, D3e each red on its own sub-arm.
  - Mutant branches `mut/pie-<M>-v1` on `d430205`; D3 and D3b were dispatched in CI before the PR.
- **#135 (open):** the `-ENOMEM` mechanism is named and fixed (#142). Arm R was green in 5 CI runs on trees with P4 and the fix; it failed 2 of 6 with P4 before it. What P4 changes to raise the rate is not explained. Close it only after repeated green runs; a recurrence is a blocker.
- **Issues open:** #135, #143 (arm C2 `net_leak_ok=0`, 1 in 202 runs, no mechanism), #116, #117, #119.
- **Review threads left open for the operator:** the Codex `docs/build_status.md` notes on #136, #138, #140, #142, #148, #150 and #154 (question in #45 6948599917).
- **Follow-ups recorded, not started:** the four other waits that ignore a handlerless SIGTERM (DDR-1206 §4); `signal_deliver` acting on default-terminate signals while a handler is active (DDR-1206 A1); the surface logging DDR-1199 proposes.
- **Lessons:**
  - Push work as soon as it builds. A container replacement lost an uncommitted build once.
  - Never `git checkout <file>` to drop a mutant from a tree with uncommitted work; revert the mutant with its own script.
  - A probe's design assumption about kernel state must be checked by running it before the gate is trusted.
  - A probe arm that a mutant can damage must restore what it snapshots whatever the call returned.
  - `hygiene_check.sh` runs `make` targets: never run it while a local gate loop is using the same build directory.
  - An arm can pass on a path the mutant does not touch. C12b survived arm L1 because `nanosleep`'s tick backstop woke the sleeper before the deadline either way (DDR-1188 A7). When a mutant survives, measure what actually ended the operation before changing a threshold.
  - In the `LOCKDEP=1` build, the hooks run on every spinlock from the first instruction of `kmain`: test the on flag before reading `%gs` (DDR-1209 D3; a mutant that read `this_cpu()` first faulted the boot).
  - Chain a commit after hygiene with `&&`, never `;`: `0bd108b` was committed while hygiene was red (a naked C `_start` in a probe; `ci-start-align-check`).
  - A gate recipe runs under `/bin/sh` (dash): no `16#` arithmetic; use `0x` literals.
  - Before a gate run, make sure `build/` holds the default kernel: a stale `LOCKDEP=1` kernel became `smoke-lockdep`'s recorded baseline once (it failed safe).
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A pull_request run that fails for a runner outage keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
  - The `github-advanced-security` check fails with "exceeded your monthly quota" (#134); not a required check.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- Merge this bookkeeping PR (docs-only rule).
- Then C1.6 P2: merge `dev/phase1` into `c16-p2-pie`, open the PR, finish D3–D3e in CI on its code head, and merge.
- Then C1.6 P3 (`PT_INTERP`, musl `ld.so`, `libc.so`) and P4 (shared text, needs C2.1); then C1.7 (KASLR; drafts #82, #84 and #85 exist, and #85 waits for the operator).
- DDR-1208 follow-up, not started: the virtqueue is not given back when a virtio-blk probe fails after queue setup (predates DDR-1208; noted on #148).

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
