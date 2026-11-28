# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-26)

- **dev/phase1 tip:** `3890e4c` (#170, C1.7 KASLR step 4, slot choice in both loaders and `smoke-kaslr`; head `324a68a`, 20/20, run 45750629607). Before it: `6b44a76` (#169, step 3, the applier), `a7b8b24` (#168, step 2, the relocation table), all three merged by the operator under DDR-1179 K4; `f868ca0` (#167, bookkeeping); `bb9939d` (#166, C1.6 P3b).
- **Kernel:** default `kernel.bin` 1,728,920 B (**40,552 B headroom**); the binding `RCU_TORTURE=1` build 1,745,304 B (**24,168 B headroom**) and `LOCKDEP=1` 1,741,208 B, all printed by CI on `324a68a` (run 45750629607), against the 1,769,472 B gate (DDR-1183 A2). **Every boot now runs slid:** the kernel is at `KERNEL_VIRT + s·2 MiB`, `s` in `[1, 510]`. Resolve a printed code address with `sym_at.sh` as before: printers unslide (`kaslr_unslide`); raw data pointers do not. Under TCG's default `qemu64` the slot comes from the TSC (`weak`). The kernel window is 0x400000..0x800000 in two PT pages (`PD_HI[s]`, `PD_HI[s+1]`). NSI: max 124 (`SYS_PSI`); next free 125. PTE software bits: 9 COW, 10 SHARED, 11 DYING, 52 SHMEM, 53 EVERW (DDR-1189 A4). DDR numbers: next free is 1211. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Standing instruction: #45 6993979920 (Nov 19).** Build every remaining chunk in checklist order, one feature per PR: DDR, code, CI gate, mutants red, merge. Merge each PR yourself under the five conditions of 6594914166 (green on the exact head, `mergeable_state` clean, mutants red or recorded not gated, CodeRabbit answered, previous PR merged). Update the checklist and this file after each merge; post a short status on #45 after each chunk. At most 3 CI runs at a time.
  - **Still wait for the operator on:** drafts #68 and #85 (the operator merges them); any key, secret, credential or trust-root step (C3.3: write the DDR, open the PR, stop); C6.2 branding.
  - **Blocker rule:** a red or inconclusive gate you cannot explain, a recurrence of an unnamed failure, or a CodeRabbit finding that shows the design is wrong: one blocker comment, mark the chunk BLOCKED, take the next unblocked chunk.
- **Order now (the operator's, 6993979920 and 7062649586):** C1.4, C1.5 (but memory PSI) and C1.7 are done; C1.6 is done but for P4 (shared text, needs C2.1). Next is Phase 2 onward by `Needs`, starting with C2.1. C11.1 stays last. C1.5 stays IN PROGRESS only for memory PSI, which waits for C2.1.
- **#135 (open):** the `-ENOMEM` mechanism is named and fixed (#142). Arm R was green in 5 CI runs on trees with P4 and the fix; it failed 2 of 6 with P4 before it. What P4 changes to raise the rate is not explained. Close it only after repeated green runs; a recurrence is a blocker.
- **Issues open:** #135, #143 (arm C2 `net_leak_ok=0`, 1 in 202 runs, no mechanism), #116, #117, #119.
- **Review threads left open for the operator:** the Codex `docs/build_status.md` notes on #136, #138, #140, #142, #148, #150, #154 and #156 (question in #45 6948599917).
- **Follow-ups recorded, not started:**
  - `smoke-thread` arm G's false-red rate. It needs at least one mid-copy fault, and the count per run varies from 3 to 14 in CI. It read 0 once, on #156's `eb210ba` (comment 7049823991), and did not recur. Making G run until a hit or a time bound is a gate change for its own PR.
  - the four other waits that ignore a handlerless SIGTERM (DDR-1206 §4); `signal_deliver` acting on default-terminate signals while a handler is active (DDR-1206 A1); the surface logging DDR-1199 proposes.
- **Lessons:**
  - A merge that applies cleanly can still be wrong: #85's `loader.c` merged without a conflict into a tree whose kernel window had grown to two PT pages, and kept drawing slot 511, which maps past the PD page (DDR-1179 A4.8). Recheck every bound a merged hunk depends on.
  - A sweep that goes by name misses printers: A4.4 unslid the printers called `rip`, and review found `freed_by`, `now_by`, `at`, `held_at` and `ret` after it (A4.9). Search for every printed value that can hold a code address.
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

- **C2.1 P1a** (this branch, `c21-p1-swap`): swap entries with a RAM-backed probe store, every user page-table walker, the `uaccess_pin` API and the `ledger_sign` fix (DDR-1190 A1.10, A1.11). Then **P1b**: the encrypted swap device and reclaim. The design is #172 `5a7a0d0` (A1 + A1.10); #171 `c6f7d33` was the C1.7 bookkeeping.
- **#72 (DDR-1182, fd access mode):** `dev/phase1` merged in again as `f57e9f4` (docs only), green 20/20 (run 46137346928), clean. The operator merges it (7111929379 item 3). Its zero-length-write Minor (`write(ro_fd, p, 0)` returns `-EBADF`) is a recorded follow-up.
- **Then C2.1** (virtual memory extensions: swap and page replacement, page cache with write-back, huge pages and `mlock`). C1.6 P4 (shared text) and C1.5's memory PSI follow it.
- The operator closes the parked drafts (7111929379 item 5); #88 is one.
- DDR-1208 follow-up, not started: the virtqueue is not given back when a virtio-blk probe fails after queue setup (predates DDR-1208; noted on #148).

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
