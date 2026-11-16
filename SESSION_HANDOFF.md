# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-16)

- **dev/phase1 tip:** `ec6187d` (#130, C1.5 P1: the time ABI, DDR-1188 P1 with Amendments 1–2). Before it: #129 `0f98ef9` (DDR-1188 Amendment 1, docs only), #128 `2f54a7e`, #125 `31edffa` (C1.4 P5b), #126 `debb6a4` (DDR-1203).
- **Kernel:** `kernel.bin` 1,655,178 B against the 1,703,936 B gate, 48,758 B headroom (measured at `ec6187d`). NSI: next free 122. DDR numbers: 1204 withdrawn (#127); **1205** is on #131 and **1206** is on branch `ddr1206-futex-fatal`; next free is 1207. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Work queue (#45 6761846242, the newest owner comment):**
  1. ~~#115~~ #118. 2. ~~#112~~ #120. 3. ~~RCU code~~ #123, #125.
  4. **C1.5 from DDR-1188:** ~~P1~~ #130. **Next: P2** (one-shot LAPIC timers, arms C and M, mutants C3 and C13), then P3–P6. Then C1.6 from DDR-1189, C1.7 from DDR-1179 (check the stale KASLR PRs #82, #84, #85 first), then deadlock detection (needs a DDR; none exists).
  - Notes for DDR-1188 (A1-5): P3's priority inheritance must move a boosted fair owner onto the RT queue and back; P6's tickless idle must wake on RT-throttle budget replenishment.
  - **Merge terms:** the five conditions of 6594914166, with the evidence posted on #45 before the merge. dev/phase1 requires branches to be up to date (strict status checks): merging one PR puts the others behind, and a base merge changes the head, so gated mutants must be re-run on the new head. Merge one gated PR at a time. At most 3 CI runs at once. Post status on #45 at the end of each item.
- **Open PRs from this round:**
  - **#131** (DDR-1205, #110): `execve` resets the FS base, plus the `PRADYOS_EXEC_FS reset=1` arm in `smoke-execve-argv`. Green on `ee42a78` before #130 merged. It needs the base merged in, a green run on the new head and mutant E1 red there (branch `mut/ddr1205-E1` is on the old head; rebuild it).
  - **`ddr1206-futex-fatal`** (DDR-1206, no PR yet): `FUTEX_WAIT` ends on a handlerless SIGTERM or SIGPIPE; `smoke-thread` arm FK; mutant F2-10. It stacks on #130. DDR-1206 §4 lists four more waits with the same gap (pipe read, pipe write, console read, `epoll_wait`); each needs its own arm and mutant.
- **Lessons from #130:**
  - Push work as soon as it builds. A container replacement lost an uncommitted P1 build in a scratchpad worktree.
  - A probe has no writable segment (`ci-probe-rodata-check`): keep shared state in a mapped page, not a global.
  - A blocking syscall must register with the signal-wake hook (`t->fx_lock`, taken by `futex_kick`), and must check `signal_interruptible`, `signal_kill_pending` and `signal_fatal_pending`. `nanosleep` had neither at first (CodeRabbit on #130).
  - A probe that signals a sleeper must first see it `THREAD_BLOCKED` through `SYS_GETPROCS`, and must check `kill`, `wait4` and the exit status.
- **Small tasks for later (6761846242 item 5: only while CI runs):**
  - **#108:** the reaper-path gate for the write-back authorization fix (S4-8 survived).
  - The surface logging DDR-1199 proposes, as its own PR.
  - The `map_core` guard: designed in DDR-1203 (#126); next is its code, gate `smoke-mapguard` and mutants M1–M3 (M4, M5 stated not gated).
- **Issues filed earlier:** #116 (exit walks), #117 (liveness-gate rebuild), #119 (`vmm_protect_range` address bound wraps). All open, not changed.
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A pull_request run that fails for a runner outage keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- Merge #131 under the merge terms, then open and merge the DDR-1206 PR.
- C1.5 P2 per DDR-1188 §3–§4.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
