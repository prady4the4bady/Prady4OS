# PRADYOS v1 master checklist

Operator-approved scope (issue #45, comments 6201120568, 6201467383, 6201619419):
**v1 contains the whole OS.** Claude works this file top to bottom.

## How to use this file (Claude: read first)

1. Find the first chunk whose status is not `DONE` and whose `Needs` are all `DONE`.
2. Build it. Every item needs a CI-registered gate (`smoke-*`, registered in `tools/ci/gate_shards.txt`) and a mutation check. No gate means not done.
3. Root-cause fixes only. No patchwork. Keep the repo clean and organized.
4. When the chunk's exit gate is green and merged: tick its boxes, set status to `DONE`, record the merge SHA, cut a release-candidate ISO, run the VirtualBox gate on it, post a short status comment on issue #45, update `SESSION_HANDOFF.md`.
5. Return to this file and take the next chunk. Do not stop between chunks. Do not ask the operator.
6. If a chunk is blocked: post one blocker comment on issue #45, mark it `BLOCKED(reason)`, continue with the next unblocked chunk.
7. Structure may change if a DDR records why. Update this file in the same PR.
8. Honesty labels in every status line: `gated`, `emulated-verified`, `not verified on real hardware`. Never label anything ASI. Quantum: interface plus simulator only, no speedup claims.
9. Operator order rule: C11.1 (Rust and build system) is ALWAYS the last chunk built. Do not pull it forward.

Status words: `TODO`, `IN-PROGRESS`, `BLOCKED(reason)`, `DONE`. The baseline for what already exists is `docs/FEATURE_STATUS.md`; update it as chunks complete.

Names marked `(name TBD)` for gates are placeholders; Claude chooses the real name.

---

## Phase 0. Housekeeping

### C0.1 Baseline and Dependabot
Status: IN PROGRESS. Needs: none.
- [x] Full gate run on the current `dev/phase1` tip; fix anything red at the root (run 39105021641, green)
- [x] Dependabot (DDR-1172): #19 closed (superseded by #48); merged #9 `3c1708d`, #7 `55b125d`, #8 `02ede03`, #30 `bca2795`, #33 `52b35f3`; #42 closed red (web-tree-sitter 0.25+ has no default export)
- [x] Open docs PRs #27, #31, #36: read, merge or close, record why (DDR-1173)
- [x] DDR-1171 rollback implementation on `rollback-ddr1171`: merged via #51 as `db5d701` (head `3795348` green, run 38989261040). Open owner question: the `sys_open` access-mode gap recorded in DDR-1171 is not fixed.
Exit gate: all existing gates green on one SHA; open-PR list clean.

---

## Phase 1. Kernel foundations

### C1.1 Signals and EINTR
Status: DONE. PR #52 merged `71d482e`; mutants M1–M4 shown red post-merge, gate fix and evidence in PR #53 merged `6edd623` (DDR-1174 §6). Needs: C0.1. Ref: DDR-1090, DDR-1174.
- [x] `sigprocmask`, `SA_SIGINFO`, `SIGCHLD`, `SA_RESTART`, EINTR semantics
Exit gate: `smoke-sigsem` (shard 6, strict).

### C1.2 Cross-CPU TLB shootdown
Status: DONE. Needs: C0.1. Ref: DDR-1075, DDR-1077, DDR-1177.
- [x] IPI-based TLB shootdown on unmap/mprotect: #57 `e6c2e16` (DDR), #58 `5e4ccf5` (code and gate). Gate green on 912ff68 (run 39141624725); M2 red (run 39141621302), M3 red (run 39141625093). mprotect is covered by C1.4 arm D. Emulated (TCG) only, not verified on real hardware.
Exit gate: `smoke-tlbshoot` (in-kernel probe, arms A-C, 4 CPUs; DDR-1177 Amendment 2). M2 and M3 shown red in CI before merge.

### C1.3 Shared memory
Status: DONE (P1, P2, P3 and P4 merged; P4 as #103 `8d69135`). Needs: C1.2. Ref: DDR-1038, DDR-1178.
- [x] P1 anonymous `MAP_SHARED` across fork: #63 merged as `a2ac698` (the DDR from #62 rides in the same merge). Gate `smoke-shm` (shard 0), arms A-F, green on `3c6865a` (run 39425747997). Mutants M1-M7 red on `80889fd` (DDR-1178 §4). Emulated (TCG) only, not verified on real hardware.
- [x] P2/P3 SysV and POSIX shared memory: decisions D1-D3 recorded (DDR-1178 Amendment 1, #70 merged as `2bda85d`); P2 is #78, merged as `287ae93` (head `1b51a36`, run [41581394086](https://github.com/prady4the4bady/Prady4OS/actions/runs/41581394086); S2-1 to S2-8 red per DDR-1178 A2.3/A2.4). P3 is #80, merged as `9105001` (head `e926ddd`, run [41636299964](https://github.com/prady4the4bady/Prady4OS/actions/runs/41636299964); S3-1, S3-2, S3-4, S3-5, S3-6 red on that head, S3-3 on `6943d97`, DDR-1178 A3.4). QEMU TCG only, not verified on real hardware
- [x] P4 shared file mappings with write-back: decisions D4-D5 recorded in the same amendment (#70, `2bda85d`); #103 merged as `8d69135` (DDR-1178 Amendment 4, NSI 119 `SYS_MSYNC`).
  - Head `5f8e31a`, run [41748812981](https://github.com/prady4the4bady/Prady4OS/actions/runs/41748812981).
  - `smoke-shm` arms FA–FJ, booted with `QEMU_SMP=4`.
  - S4-1 to S4-7 and S4-9 are red on their own arms (DDR-1178 A4.9). S4-2 is red on FH, so the msync TLB flush is gated.
  - **Not gated:**
    - ~~the write-back authorization-context fix: S4-8 survived, follow-up #108~~ **gated since #134 `03a3326`** (DDR-1178 §A4.10): arm FR makes the reaper do the last `mm_put`. Head `a1c2a31` green (run [43867788293](https://github.com/prady4the4bady/Prady4OS/actions/runs/43867788293)); S4-8 red on FR only (`reaper=0x00`, run [43867799425](https://github.com/prady4the4bady/Prady4OS/actions/runs/43867799425)). Not gated: a reaper release that fails for another reason than authorisation;
    - the io_uring growth refusal;
    - J/L cross-CPU false passes;
    - two concurrent first mappers.
  - QEMU TCG only, not verified on real hardware.
Exit gate: `smoke-shm` (P1 arms A-F; P2-P4 arms are added with their parts).

### C1.4 Threads and synchronization
Status: IN PROGRESS. Needs: C1.1, C1.2, C1.3. Merged: #76 `3358b35` (P1c, DDR-1181: `SYS_THREAD_CREATE`/`EXIT`, thread groups, `smoke-thread`), #91 `f0e1bb6` (P1d, DDR-1186: `SYS_SCHED_CPU`, cross-CPU arms D/E/M/G). #81 `cc99745` (P2, DDR-1181 Amendment 1: `SYS_FUTEX`), #101 `9306963` (P3a, DDR-1193: `psync.h`), #102 `ca53351` (DDR-1195: futex timeout +1 tick, FC/FF/FG witnesses; head `6f59e7c` green on run [41573162001](https://github.com/prady4the4bady/Prady4OS/actions/runs/41573162001)). #114 `97f0397` (DDR-1200, RCU design, docs only). Two defect fixes from operator #45 6761846242: #120 `9dba21f` (DDR-1202, #112: `mprotect` copies a shared private page before adding write; head `8b676d3` green on run [43139028277](https://github.com/prady4the4bady/Prady4OS/actions/runs/43139028277); mutants P1–P3 red on arms F, H, G; `smoke-shm` arm E shown not to gate the SHMEM exemption) and #118 `baac4dd` (DDR-1201, #115: the timer-tick sweep starts on the ring; head `5bb8651` green on run [43145274402](https://github.com/prady4the4bady/Prady4OS/actions/runs/43145274402); `smoke-zanchor` arms A and B; Z1 red on B, Z2 red on A, both on that head). The `map_core` guard, 6761846242 item 5: #137 `2506c94` (DDR-1203 and Amendment 1: a USER leaf is refused under a kernel-shared top-level entry or into the kernel master; `smoke-mapguard` arms G-A to G-D; head `051c112` green on run [44105667165](https://github.com/prady4the4bady/Prady4OS/actions/runs/44105667165); M1 red on G-A and G-B, M2 on G-C, M3 on G-A, G-B and G-C). QEMU only, not verified on real hardware. Every run below is on `ubuntu-24.04`, pinned by #97 `757983b` (DDR-1191). Cross-CPU TLB coherence for threads: gated (`smoke-thread` D, E, M), QEMU only, not verified on real hardware.
- [x] futex: #81 `cc99745` (DDR-1181 Amendment 1, `SYS_FUTEX` NSI 117). `smoke-thread` arms FA–FJ green on head `547e1c5` (run [41272172932](https://github.com/prady4the4bady/Prady4OS/actions/runs/41272172932)); F2-1 to F2-8 each red on its own arm on that head (runs 41274796623, 41274798202, 41275022079, 41277251600, 41277253535, 41279654646, 41279656521, 41282177441; table in #81 comment 6554928498). Merged head `5997083` (only CI files and docs added) green on run [41299374169](https://github.com/prady4the4bady/Prady4OS/actions/runs/41299374169) attempt 2; attempt 1 failed `smoke-part` on a console line split by another CPU (DDR-1194). The known limit of DDR-1181 A1.3 (a handlerless default-terminate signal other than SIGKILL did not end a FUTEX_WAIT) is closed by #133 `0f16c16` (DDR-1206 and its Amendment 1): arms FK and FL. Head `2996628` green (run [44055028121](https://github.com/prady4the4bady/Prady4OS/actions/runs/44055028121)); F2-10 red on FK only (run [44055045345](https://github.com/prady4the4bady/Prady4OS/actions/runs/44055045345)), F2-11 red on FL only (run [44055053599](https://github.com/prady4the4bady/Prady4OS/actions/runs/44055053599)). Not done: four other waits with the same gap (DDR-1206 §4), and `signal_deliver` acting on default-terminate signals while a handler is active. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] pthreads / `CLONE_VM`, thread-local storage. Kernel part: #76 `3358b35` (`CLONE_VM` threads, per-thread TLS, CLEARTID, group exit; `smoke-thread` A, B, C, F). User-space library: #111 `2a508d5` (DDR-1197 and its Amendment 1). It is `pthr.h` on `psync.h`, gated by `smoke-thread` arms PA–PO. Head `5bab41c` green (run 42353336050); mutants R1–R8 red, each on its own arm (runs 42353410272–42361007239). Not gated: a fork during `pthr_create`, a self-detach, a read below the stack, a frame larger than one page (Amendment 1 items 9–11). ELF `__thread` is not part of it and goes with C1.6's loader. Emulated (TCG) only, not verified on real hardware.
  - **#135, an intermittent `PTHR_R` "R create" refusal (open):** #141 `a50e509` (DDR-1197 Amendment 2) makes arm R print the refusing step and its return code; head `8bcc0fa` green (run [44342915679](https://github.com/prady4the4bady/Prady4OS/actions/runs/44342915679)); no gate change, so no mutants. It named the step: `SYS_THREAD_CREATE` returned `-ENOMEM` (step 6). #142 `2ca1ea2` (DDR-1207) removes the named mechanism. The kheap large-allocation registry was full at 256 entries, because each thread holds two (its tcb and kernel stack) until its RCU grace period ends. It now holds 1024. Gate `smoke-thread` arm KL; head `473b983` green (run [44387890143](https://github.com/prady4the4bady/Prady4OS/actions/runs/44387890143)); mutant K1 red on KL (`got=182 ok=0`, run [44387887320](https://github.com/prady4the4bady/Prady4OS/actions/runs/44387887320)). #135 stays open until arm R is green over repeated CI runs. QEMU only, not verified on real hardware.
  - `execve` resets the FS base (the DDR-1197 §10 item, #110): #131 `b2181de` (DDR-1205). `smoke-execve-argv` requires `PRADYOS_EXEC_FS reset=1`. Head `942e452` green (run [43832412537](https://github.com/prady4the4bady/Prady4OS/actions/runs/43832412537)); mutant E1 red on that gate (run [43832419536](https://github.com/prady4the4bady/Prady4OS/actions/runs/43832419536)). Not changed: the user GS base. QEMU (TCG) only, not verified on real hardware.
- [x] mutex, semaphore, condition variable, barrier (DDR-1193; priority inheritance moved to C1.5, DDR-1193 D1): #101 `9306963`, `user/include/psync.h`. `smoke-thread` SA, SW, SB, SC, SH, SD green on `2bd99ca` (run [41364910443](https://github.com/prady4the4bady/Prady4OS/actions/runs/41364910443)) and on the merged head `d35da2e` (run [41384755109](https://github.com/prady4the4bady/Prady4OS/actions/runs/41384755109)). Y1–Y6 each red on its own arm (runs 41367275758, 41371310617, 41371312750, 41374490031, 41375830310, 41380005173; DDR-1193 Amendment 1). SA catches Y1 only probabilistically (one lost update); MTTCG not verified. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] RCU. Designed in DDR-1200: kernel RCU (IF-off read sides, tick-counter grace periods), first consumer the timer-tick ring sweep, gate `smoke-rcu` with mutants U1–U9 (U7, U9 and the locked increment stated possibly not gated). The code is authorized by #45 6761846242 item 3, which required #115 merged first (done: #118 `baac4dd`). Split into two parts (DDR-1200 Amendment 1). **P5a merged: #123 `3f9be84`** (the grace-period core, `smoke-rcu` arms RA/RB/RC; head `57eb09f` green on run [43395743995](https://github.com/prady4the4bady/Prady4OS/actions/runs/43395743995); U1, U2, U3 red on that head; the locked tick increment stated not gated). **P5b merged: #125 `31edffa`** (the lockless timer-tick sweep on RCU and the deferred tcb free, DDR-1200 Amendments 2–3; `smoke-rcu` arms RD/RE/RF; head `4a38f9b` green on run [43689673463](https://github.com/prady4the4bady/Prady4OS/actions/runs/43689673463); on that head U4 red on RD (`tick_lock=14061`, run 43689812721), U6 red on RF (`bad_node=3`, run 43689814920), U8 red on RE (`sat_acts_ok=0`, run 43692434958), U9 red on RF (`hold_rounds=0`, run 43695818932); U5 and U7 survived on `ba67511` and are recorded not gated in Amendment 3). P5b changes the default build (the sweep runs on every tick and `sched_free_tcb` defers through RCU); only the counters, RF hooks and poison are `RCU_TORTURE`-only. Default `kernel.bin` measured at `31edffa`: 1,638,794 B, sha256 prefix `23ff2ca01b49b863`. QEMU only, not verified on real hardware.
- [x] deadlock detection (lock-order checking): #152 `b8ab375` (DDR-1209). A `LOCKDEP=1` build only; the shipped kernel checks nothing and is byte-identical to `dev/phase1` (local `cmp`, sha256 `26f99e18…`). Gate `smoke-lockdep` (ci.yml `flag-gates`), arms LA–LE; arm LA reads the line `ldsum` prints after freezing the checker at about 240 s. Head `b555f78` green, 19 of 19 jobs (run [44727605346](https://github.com/prady4the4bady/Prady4OS/actions/runs/44727605346)). Mutants LD1–LD7 on `677b3db` (the code head; `b555f78` is docs only), each red only in `flag-gates (smoke-lockdep)` on its own arm: LD1 on LB/LC/LE (run 44725800972), LD2 on LC (44735739624), LD3 on LD (44748203949), LD4 on LE, and LB/LC (44748207799), LD5 on LA (`LOCKDEP INVERSION`, 44725797663), LD6 on LA's order rule (44732534223), LD7 on LA's missing final line (44733069969). Not covered (DDR-1209 §6): interrupt safety, sleeping locks, ordered same-class nesting, locks taken after the freeze. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] REQUIRED (DDR-1177 Amendment 2, #45/6251086742 item 3): TLB shootdown arm D, `mprotect` read-only on one CPU makes a write on another CPU fault. #91 `f0e1bb6` (DDR-1186): green on head `a4a508a` (run [41224958867](https://github.com/prady4the4bady/Prady4OS/actions/runs/41224958867)); mutant T4 red on D only (`prot_stale=19570159`, run [41228026512](https://github.com/prady4the4bady/Prady4OS/actions/runs/41228026512)). QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] REQUIRED (same): TLB shootdown arm E, the user-space `munmap`/remap stale-read test with `CLONE_VM` threads. #91 `f0e1bb6`: mutant T5 red on E (`stale=9118354`, run [41228028822](https://github.com/prady4the4bady/Prady4OS/actions/runs/41228028822)). QEMU only, not verified on real hardware.
- [x] REQUIRED (same, item 5): a gate that catches DDR-1177 mutant M4 (frame freed before every CPU acked), via a poison-on-free reader thread. Owner: the agent working this checklist. M4 must be shown red. #91 `f0e1bb6`: arm M; mutant T6 (M4) red on M only (`poison=18942`, run [41230409176](https://github.com/prady4the4bady/Prady4OS/actions/runs/41230409176)). QEMU only, not verified on real hardware.
Exit gate: threads and lock gates (names TBD) incl. race stress, plus the three DDR-1177 items above.

### C1.5 Scheduler classes
Status: IN PROGRESS (P1 to P6 merged; DDR-1188 §3; memory PSI waits for C2.1). Needs: C1.4.
- [x] **P1, the time ABI** (DDR-1188 P1, Amendments 1–2): #130 `ec6187d`. `CLOCK_MONOTONIC` and `CLOCK_REALTIME` on the TSC through the vDSO (K7), `SYS_CLOCK_GETTIME` (120), `SYS_NANOSLEEP` (121), the per-AP TSC skew measurement, and gate `smoke-schedclass` arms A, B, B2 and B3.
  - Green on the final head `5317bf5` (run [43775518109](https://github.com/prady4the4bady/Prady4OS/actions/runs/43775518109)).
  - Mutants, each built on `5317bf5` and red: C1 on arm A (`back=110`, run 43790409362); C2 on arm B (`slept_ok=1`, run 43790413646); C2b on arm B2 and also B3 (`usr1_ms=5008 eintr=0`, run 43775724179); C2c on arm B3 only (`term_ms=5009`, run 43775726375).
  - Not gated: C1b (the reader's seqlock retry, A1-3) and A2-2 (the `lfence` before the reader's TSC sample).
  - P1 does not tick any of the three items below: it is only the clock part of the third. P2 (one-shot timers) to P6 (tickless) remained after it.
  - QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] **P2, one-shot LAPIC timers** (DDR-1188 Amendment 3): #136 `5086ec6`. `nanosleep` arms a one-shot on the LAPIC count between ticks (T1–T5: an 8-slot per-CPU table, a budget of 1 000 one-shots per thread per 1 s window, 50 µs spacing). Gate `smoke-schedclass` arms C and M.
  - Green on the final head `a10315a` (run [44087243331](https://github.com/prady4the4bady/Prady4OS/actions/runs/44087243331): `early=48 of 50`, `sleeps=1042`).
  - Mutants on `a10315a`: C3 red on arm C and on arm M (`early=6`, `sleeps=102`, run 44087238859); C13 red on arm M only (`sleeps=2040`, run 44087247529).
  - Not gated (Amendment 3): T5 spacing, the table-full path, a cancel from another CPU, the tick delay, the five review fixes. TSC-deadline mode is not built.
  - Open on review: T5's floor is measured from the previous expiry and the count rounds down (under one LAPIC count short). P3 rounds up and rewords T5 (#45 6950265363).
  - P2 does not tick the three items below either; it is the high-resolution part of the third. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] real-time classes (FIFO, RR, deadline), with `kmutex` priority inheritance (DDR-1193 D1). **Done in P3 and P4:**
  - **P3** (DDR-1188 Amendment 4): #138 `1c4e6bc`. `SCHED_FIFO` and `SCHED_RR` ahead of the fair class on the one run queue, wake-up preemption on the tick, the 19-of-20-tick RT throttle (K2), `RES_SCHED` with `SYS_SCHED_SETSCHED` (122) and `SYS_SCHED_GRANT` (123), one-level `kmutex` priority inheritance (DDR-1193 D1).
  - Green on the final head `45e8319` (run [44217157028](https://github.com/prady4the4bady/Prady4OS/actions/runs/44217157028): D `late=0 of 50`, N `late=2 of 50`, G `w=5,5,5,5,5`, P `wait_ms=245`).
  - Mutants on `45e8319`, each red on its own arm: C4 on D (and F), C5 on E, C6 on F, C7 on G, C8 and C8b on H, C14 on N (and P), C16 on P (runs 44217188590, 44225784701, 44225787466, 44225881136, 44231915094, 44231917549, 44237822831, 44217191755).
  - Not gated (Amendment 4): cross-CPU priority, RT priority on steal, a successful grant and its spread to threads, the missed-copy fork case, transitive inheritance, priority-ordered hand-off, the higher-priority head re-queue, the T5 floor and per-window bound. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
  - **P4** (DDR-1188 Amendment 5): #140 `449cd7f`. `SCHED_DEADLINE` (policy 3 of `SYS_SCHED_SETSCHED`, no new NSI) for pinned threads: EDF in front of the RT classes, a constant-bandwidth server charged by tick sampling, the CBS wake rule, and admission at 0.90 per CPU (K4). Arms I, J and Q; the probes compile at `-Os` (the `smoke-rcu` size fix).
  - Green on the final head `b869d4e` (run [44398576450](https://github.com/prady4the4bady/Prady4OS/actions/runs/44398576450): I `rc=0,0,-16,0,0`, J `afirst=100 of 100`, Q `w=90,90,90`).
  - Mutants on `b869d4e`, each red on its own arm only: C9 on I (`rc=0,0,0,0,0`, run [44398599436](https://github.com/prady4the4bady/Prady4OS/actions/runs/44398599436)), C10 on J (`afirst=0 of 100`, run [44403448352](https://github.com/prady4the4bady/Prady4OS/actions/runs/44403448352)), C17 on Q (`w=5,5,5`, run [44407139339](https://github.com/prady4the4bady/Prady4OS/actions/runs/44407139339)).
  - Not gated (Amendment 5): deadline inheritance through a `kmutex`, the work-conserving run of a CBS-throttled thread, the wake rule's keep-the-deadline branch, `-EBUSY`/`-EINVAL` paths, share release on `tcb` free, preemption between two deadline threads. Two minor review notes (CodeRabbit on #140) are fixed on P5. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [ ] pressure stall information (CPU, memory, I/O). **CPU and I/O done (P5); memory waits for C2.1 (DDR-1188 §2):**
  - **P5** (DDR-1188 Amendment 6): #145 `fa96706`. `some` over a 100-tick sliding window: per CPU (a READY thread waiting, excluding a thread queued by its own `yield()`), and system-wide for virtio-blk reads (from submit until completion or timeout). `SYS_PSI` (NSI 124). It also carries two P4 review fixes (`rt_tick` spent budget, the `rq_pop` K2 fallback).
  - Green on the final head `40f1f39` (run [44482663825](https://github.com/prady4the4bady/Prady4OS/actions/runs/44482663825): K `load=100 idle=0`, O `peak=7 after=0`).
  - Mutants on `40f1f39`: C11 red on K (`load=0`, run [44482747067](https://github.com/prady4the4bady/Prady4OS/actions/runs/44482747067)), C11b red on K0 and O (`idle=100`, `after=51`, run [44482849701](https://github.com/prady4the4bady/Prady4OS/actions/runs/44482849701)), C15 red on O (`peak=0`, run [44488552525](https://github.com/prady4the4bady/Prady4OS/actions/runs/44488552525)).
  - Not gated (Amendment 6): the yield exclusion and its timing, I/O sampling only on CPU 0, writes and flushes, the busy-lock sample, the two carried P4 fixes, op 5 keeping its buffer after a timeout. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
  - **DDR-1208** (found reviewing P5): #148 `43d62c1`. A timed-out virtio-blk request no longer leaves the device holding caller memory: per-slot 32 KiB bounce buffers, and a timed-out slot is held until its late completion. Asked for by the operator. Green on the final head `4298fde` (run [44540005938](https://github.com/prady4the4bady/Prady4OS/actions/runs/44540005938)): `smoke-vblklate` arms Q `held=1 done_stale=0 late=1 freed=1 done_real=1` and B `via=1 sig=1`. Mutants on `4298fde`, each red on its own arm only: Q1 (`held=0`, run [44540119698](https://github.com/prady4the4bady/Prady4OS/actions/runs/44540119698)), B1 (`via=0`, run [44549127906](https://github.com/prady4the4bady/Prady4OS/actions/runs/44549127906)). Not gated (DDR-1208 §3): a real late completion after a 5 s stall, the 64-sector split, eight orphaned slots, the probe failure path. It needed DDR-1183 Amendment 1 (#147 `3971031`, stage-2 window 52 -> 53 chunks, run [44537776868](https://github.com/prady4the4bady/Prady4OS/actions/runs/44537776868)). QEMU (TCG, 4 vCPU) only, not verified on real hardware.
- [x] CPU frequency and tickless timers, high-resolution clocks. **Done in P1, P2 and P6:** the clocks are P1 (`CLOCK_MONOTONIC`/`CLOCK_REALTIME` on the TSC), the high-resolution timers P2 (LAPIC one-shots); tickless is P6. "CPU frequency" is a CPUID report only (`[cpufreq] hwp= eist=`, HWP and EIST bits), with no frequency control (DDR-1188 A7 T8).
  - **P6** (DDR-1188 Amendment 7): #150 `0ae6251`. An idle AP stops its tick for at most 1 s (`NOHZ_MAX_TICKS`), counting to the earliest sleeper's deadline when that comes first; the BSP never stops. The clock catches up by `max(k, 1)` ticks, PSI records the skipped ticks as unstalled, and RCU treats a stopped CPU as quiescent. Probe-only ops 6–8 of `SYS_SCHED_CPU` (isolate, timer-interrupt count, timed `synchronize_rcu`). Gate `smoke-schedclass` arms L, L1, L2.
  - Green on the final head `304a781` (run [44687523048](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687523048)).
  - Mutants on `304a781`, each red on its own arm only: C12 on L (run [44687627569](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687627569)), C12b on L1 (run [44687624768](https://github.com/prady4the4bady/Prady4OS/actions/runs/44687624768)), C12c on L2 (run [44691904269](https://github.com/prady4the4bady/Prady4OS/actions/runs/44691904269)).
  - L1 was redefined after C12b survived on the previous head `d83db5b` (run 44676499866): with a 20 ms sleep, nanosleep's tick backstop woke the sleeper at or before its deadline, so the arm could not see whether the stop counted to it. L1 now sleeps 20.1 ms and needs 40 of 50 wakes under 2 ms late (DDR-1188 A7).
  - Not gated (Amendment 7): the 1 s bound as a backstop, the deadline clock and the K2 window across a stop, the PSI catch-up, the mailbox check before a stop, the CPUID report, the isolate op's drain. QEMU (TCG, 4 vCPU) only, not verified on real hardware.
Exit gate: `smoke-schedclass` (arms A, B, B2, B3, C, M, D, E, F, G, H, HA, N, P, I, J, Q, K, K0, O, L, L1, L2). C1.5 stays IN PROGRESS only for memory PSI (C2.1).

### C1.6 Dynamic linking and loader
Status: IN-PROGRESS (P1 merged, #154; P2 to P4 open, DDR-1189). Needs: C1.3.
- [ ] ELF dynamic loader, relocation, shared libraries, shared text pages
- [ ] user-space ASLR. **P1 (stack top and mmap base) merged:** #154 `0d464e1` (DDR-1189 Amendment 1). Gate `smoke-dynlink` arms A and B (shard 8). Head `da6ce27` green, 19 of 19 jobs (run [44778658685](https://github.com/prady4the4bady/Prady4OS/actions/runs/44778658685)). Mutants D1, D2 and D2b ran on `9b99b13`, the code head; `da6ce27` only merges docs. Each was red only in shard 8, on its own arm: D1 on A (run 44761801440), D2 on B (44761806644), D2b on B (44768583641). The program image moves with P2 (static PIE), which closes this item. P2's design, DDR-1189 Amendment 2, is on branch `c16-p2-pie` until its PR merges. Strength is bounded by DDR-1189 L3 (16 and 18 bits). QEMU (TCG) only, not verified on real hardware.
Exit gate: dynamic-linking gate (name TBD).

### C1.7 KASLR
Status: TODO (was deferred, now in scope). Needs: C0.1. Ref: DDR-1051.
Exit gate: KASLR gate (name TBD).

---

## Phase 2. Memory and storage

### C2.1 Virtual memory extensions
Status: TODO. Needs: C1.3.
- [ ] swap device, page replacement (clock/LRU-approximation), working-set control
- [ ] page cache with dirty-page write-back
- [ ] huge pages, `mlock`
Exit gate: memory-pressure gate (name TBD).

### C2.2 VFS layer and file semantics
Status: TODO. Needs: C2.1.
- [ ] one VFS over FAT32, SFS, ext4; inode and dentry caches; hard links
- [ ] permissions, ACLs, file locking
Exit gate: VFS and permissions gates (names TBD).

### C2.3 Filesystem completion
Status: TODO. Needs: C2.2.
- [ ] SFS large files (> 16 KiB), SFS on-disk free tree. Ref: DDR-1100.
- [ ] ext4 write (journaled)
- [ ] snapshots (copy-on-write), TRIM, I/O scheduler
- [ ] durable on-disk audit log (unblocked by SFS large files)
Exit gate: large-file, ext4-write, snapshot and durable-audit gates (names TBD).

---

## Phase 3. Identity and security

### C3.1 Accounts and authentication
Status: TODO. Needs: C2.2.
- [ ] users, groups, login, password hashing
Exit gate: login gate (name TBD).

### C3.2 Isolation
Status: TODO. Needs: C1.4, C3.1.
- [ ] namespaces (process, network, mount, hostname, IPC, user)
- [ ] cgroups-style resource groups (CPU quota, memory limit, I/O)
- [ ] op-level syscall filtering (account for `io_uring` bypass)
- [ ] mandatory access control hooks
Exit gate: isolation and escape-attempt gates (names TBD).

### C3.3 Cryptographic platform
Status: TODO. Needs: C3.1.
- [ ] disk encryption, TPM support (measured boot, sealed keys)
- [ ] secure boot, code signing of binaries and updates
- [ ] stack canaries, control-flow integrity
Exit gate: encrypted-boot and signed-binary gates (names TBD).

### C3.4 Network security
Status: TODO. Needs: C4.2 (network stack).
- [ ] general stateful firewall and traffic shaping (beyond egress allowlist)
Exit gate: firewall gate (name TBD).

---

## Phase 4. Devices and connectivity

### C4.1 USB
Status: TODO. Needs: C1.4. Ref: xHCI keyboard gated (DDR-1167).
- [ ] hubs, mouse, mass storage, descriptors hardening
Exit gate: usb-hub, usb-mouse, usb-storage gates (names TBD).

### C4.2 Network stack
Status: TODO. Needs: C1.4. Ref: DDR-988, DDR-1104.
- [ ] IPv6, TLS, TAP, completion of deferred net work
Exit gate: ipv6, tls, tap gates (names TBD).

### C4.3 Wireless
Status: TODO. Needs: C4.2, C3.3.
- [ ] Wi-Fi, Bluetooth (honest label: not verified on real hardware if only emulated)
Exit gate: emulated wireless gate (name TBD).

### C4.4 Storage IRQ and audio
Status: TODO. Needs: C1.4.
- [ ] NVMe completion IRQ
- [ ] HDA audio and mixer
Exit gate: nvme-irq, audio gates (names TBD).

### C4.5 GPU, display, power
Status: TODO. Needs: C1.5.
- [ ] GPU acceleration path (virtio-gpu first), multi-monitor
- [ ] power management, suspend, battery
- [ ] touch and accessibility input
- [ ] printing
Exit gate: gpu, suspend, accessibility gates (names TBD).

---

## Phase 5. Platform services

### C5.1 Packages and updates
Status: TODO. Needs: C3.3, C2.3.
- [ ] package manager, content-addressed signed packages, signed atomic updates, rollback
- [ ] configuration store
- [ ] crash dumps
- [ ] i18n and locale
Exit gate: update-and-rollback gate (name TBD).

### C5.2 Virtualization
Status: TODO. Needs: C2.1, C3.2.
- [ ] hypervisor support (VT-x, AMD-V), checkpoint and restore
Exit gate: guest-boot gate (name TBD).

---

## Phase 6. Desktop and branding

### C6.1 Desktop completion
Status: TODO. Needs: C4.5.
- [ ] taskbar magnify, Horizon animated mesh, agent panel graphs (CPU sparkline, memory graph, action histogram)
- [ ] Sovereign/Manual switch polish
Exit gate: extends smoke-mode, smoke-horizon, smoke-agentpanel.

### C6.2 Original branding
Status: TODO. Needs: none (run in parallel).
- [ ] logo, wordmark, icon set from scratch in `assets/brand/`, SVG source, documented grid, stroke and radius
- [ ] no copying or tracing of any existing OS or icon pack; DDR records the rule
- [ ] icon gate: exists, fits grid, renders at 16/24/32/64 px
Exit gate: icon gate (name TBD). Operator reviews by eye before v1.

---

## Phase 7. Agent layer (AETHER)

### C7.1 Agent capabilities
Status: TODO. Needs: C3.2. Ref: Block 3.
- [ ] UI-state reading, input injection
- [ ] sandboxed shell and browser
- [x] rollback on rejection (DDR-1171): #51 `db5d701`, gate `smoke-rollback`. Emulated (TCG) only, not verified on real hardware.
- [ ] kernel policy hooks: agents propose tunings; kernel validates against fixed bounds and audits; agents never run scheduler, memory or security decisions directly
Exit gate: each capability has a gate plus a negative gate.

### C7.2 Domain agents
Status: TODO. Needs: C7.1. Ref: F#66-F#75.
- [ ] domain agents built on `user/agent_base.c`
Exit gate: one gate per agent.

### C7.3 Inference
Status: TODO. Needs: C4.2, C3.2.
- [ ] local inference (Ollama) gated where possible
- [ ] cloud inference (K1, DDR-793 now in scope) with egress and privacy gates
- [ ] quantum interface plus simulator path (K4), no speedup claims
Exit gate: cloud-bridge egress gate, privacy gate.

### C7.4 Adversarial gates
Status: TODO. Needs: C7.1.
- [ ] prompt injection, self-approval, budget overrun, egress bypass, sandbox escape
Exit gate: adversarial gate set.

---

## Phase 8. Architectures and chipset variants

### C8.1 Variants
Status: TODO. Needs: Phase 1 through 5 stable.
- [ ] x86_64 CPU-family variants (runtime dispatch beyond ERMS/NX/SMEP/SMAP/MCE)
- [ ] aarch64 and riscv64 as QEMU-gated builds (record limits)
- [ ] Apple Silicon: only what can be built and tested without hardware; else document as not verified
Exit gate: per-variant boot gate.

---

## Phase 9. Test and harden

### C9.1 Sanitizers
Status: TODO. Needs: Phases 1 to 7.
- [ ] KASAN-style, UBSAN, KCSAN-style race detection, uninitialized-memory checks, all gated

### C9.2 Fuzzing
Status: TODO. Needs: C9.1.
- [ ] coverage-guided syscall fuzzer (syzkaller-style: descriptions, VM executor, corpus, triage)
- [ ] parser and driver fuzzing (ELF loader, filesystems, USB descriptors, network stack, IPC)
- [ ] run until coverage plateaus; record hours

### C9.3 Security sweep
Status: TODO. Needs: C9.2.
- [ ] dependency and secret scanning
- [ ] every bug: root-cause fix, regression gate, mutation check
- [ ] report fuzz hours, bugs found and fixed, anything still open; never claim 'no vulnerabilities'

---

## Phase 10. Release

### C10.1 v1 ISO
Status: TODO. Needs: every chunk except C11.1 DONE, or recorded as an honestly-labelled exception.
- [ ] one release SHA, full regression green
- [ ] VirtualBox install-and-boot gate green
- [ ] readiness report on issue #45: every feature labelled gated / emulated-verified / not verified on real hardware
- [ ] tag v1.0.0, promote `dev/phase1` to `main`, attach ISO and SHA-256 to the release

---

## Phase 11. Last in build order (operator rule)

### C11.1 Rust and build system (was C8.2)
Status: TODO. Needs: C10.1. Always built last.
- [ ] Rust only where a DDR shows a measured safety or speed win
- [ ] Makefile/CMake hybrid only if a DDR justifies it
- [ ] Because this lands after hardening and the v1 tag: any merged change re-runs the C9.1 to C9.3 gates and the C10.1 release gate, and ships as a patch release (v1.0.x), never silently
Exit gate: DDR with measured results; C9 and C10 gates green again on the new SHA.
