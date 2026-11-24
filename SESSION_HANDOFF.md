# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-23)

- **dev/phase1 tip:** `8843b8a` (#160, DDR-1210: the user-trap report written as one locked `kwrite`, so it no longer splices into another CPU's line; head `63bbe5d`, 20/20; the DDR and code share commit `c86485a`, waived by the operator, #45 7111929379 item 4). Before it: `1958874` (#163, bookkeeping), `037a9e4` (#162, the C1.6 P3a follow-up, DDR-1189 Amendment 4).
- **Kernel:** default `kernel.bin` 1,716,626 B (20,078 B headroom); the binding `RCU_TORTURE=1` and `LOCKDEP=1` builds 1,733,010 B (**3,694 B headroom, less than one 4 KiB page**), all printed by CI on `a3acf52` (run 45212716257), against the 1,736,704 B gate. The PT_HI ceiling (image plus BSS at or below phys 0x600000) is build-enforced (DDR-1183 A1). NSI: max 124 (`SYS_PSI`); next free 125. PTE software bits: 9 COW, 10 SHARED, 11 DYING, 52 SHMEM, 53 EVERW (DDR-1189 A4). DDR numbers: 1210 is taken by #160 (open); next free is 1211. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Standing instruction: #45 6993979920 (Nov 19).** Build every remaining chunk in checklist order, one feature per PR: DDR, code, CI gate, mutants red, merge. Merge each PR yourself under the five conditions of 6594914166 (green on the exact head, `mergeable_state` clean, mutants red or recorded not gated, CodeRabbit answered, previous PR merged). Update the checklist and this file after each merge; post a short status on #45 after each chunk. At most 3 CI runs at a time.
  - **Still wait for the operator on:** drafts #68 and #85 (the operator merges them); any key, secret, credential or trust-root step (C3.3: write the DDR, open the PR, stop); C6.2 branding.
  - **Blocker rule:** a red or inconclusive gate you cannot explain, a recurrence of an unnamed failure, or a CodeRabbit finding that shows the design is wrong: one blocker comment, mark the chunk BLOCKED, take the next unblocked chunk.
- **Order now (the operator's, 6993979920 and 7062649586):** C1.5 P5 and P6 and deadlock detection (C1.4) are done; C1.6 is in progress (P1, P2, P3a and its A4 follow-up done); then C1.7, then Phase 2 onward by `Needs`. C11.1 stays last. C1.5 stays IN PROGRESS only for memory PSI, which waits for C2.1.
- **#135 (open):** the `-ENOMEM` mechanism is named and fixed (#142). Arm R was green in 5 CI runs on trees with P4 and the fix; it failed 2 of 6 with P4 before it. What P4 changes to raise the rate is not explained. Close it only after repeated green runs; a recurrence is a blocker.
- **Issues open:** #135, #143 (arm C2 `net_leak_ok=0`, 1 in 202 runs, no mechanism), #116, #117, #119.
- **Review threads left open for the operator:** the Codex `docs/build_status.md` notes on #136, #138, #140, #142, #148, #150, #154 and #156 (question in #45 6948599917).
- **Follow-ups recorded, not started:**
  - `smoke-thread` arm G's false-red rate. It needs at least one mid-copy fault, and the count per run varies from 3 to 14 in CI. It read 0 once, on #156's `eb210ba` (comment 7049823991), and did not recur. Making G run until a hit or a time bound is a gate change for its own PR.
  - the four other waits that ignore a handlerless SIGTERM (DDR-1206 §4); `signal_deliver` acting on default-terminate signals while a handler is active (DDR-1206 A1); the surface logging DDR-1199 proposes.
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
  - Let reviews settle before dispatching mutants: on #156 two rounds of review fixes changed the code head, and the mutants ran three times (v1, v2, v3).
  - Before a gate run, make sure `build/` holds the default kernel: a stale `LOCKDEP=1` kernel became `smoke-lockdep`'s recorded baseline once (it failed safe).
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A pull_request run that fails for a runner outage keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
  - The `github-advanced-security` check fails with "exceeded your monthly quota" (#134); not a required check.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- Merge this bookkeeping PR (docs-only rule).
- **#72 (DDR-1182, fd access mode), operator 7062649586 item 2:** merge `dev/phase1` into it, resolve the conflicts, re-run `smoke-accmode` and mutants M1–M8 on the new head, post the evidence on #72. Do not merge it: the operator does.
- **Operator decisions, #45 7111929379 (Nov 23):**
  - Merge authority for kernel PRs is restored under the five conditions of 6594914166 (item 1). Still the operator's to merge: #72, any boot-loader PR, keys and trust roots, and the #85 replacement.
  - **#164 (DDR-1183 A2, the size gate: 54 chunks and a 4 MiB kernel window)** is open for the operator to merge (item 2). Its mutants W1-W6 and S1' are dispatched. It comes before P3b and any other kernel growth.
  - **#72:** after #164 merges, merge `dev/phase1` into it, re-run `smoke-accmode` and M1-M8, and post the evidence; the operator merges it (item 3).
  - The operator closes the parked drafts (item 5).
- Then C1.6 P3b (musl `ld.so`/`libc.so`, the syscall shim, arms D, E, G; arm C4's image names `/lib/ld-musl-x86_64.so.1` and must be re-pointed when that file is installed), then C1.7 (KASLR rebuilt as fresh PRs in DDR-1179's order, operator 7062649586 item 3). C1.6 P4 (shared text) needs C2.1, which is in Phase 2, so it comes after C2.1, not before C1.7.
- **Size gate first (3,694 B left in the binding builds):** the next 4 KiB page crossing fails CI. A DDR-1183 window raise (54 chunks, PT_HI in both `boot/stage2/stage2.asm` and `boot/uefi/loader.c`, proposed on #45 7091904894) must land before P3b or any other kernel growth; #72 is already blocked on it. Waits for the operator's choice.
- DDR-1208 follow-up, not started: the virtqueue is not given back when a virtio-blk probe fails after queue setup (predates DDR-1208; noted on #148).

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
