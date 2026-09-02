# PRADYOS — Claude Code Session Rules (CLAUDE.md)

PRADYOSSovereign Edition: a from-scratch, bare-metal, AI-native OS for x86_64.
**This file governs every session. Read it in full before touching anything.**
**DEADLINE: 2026-08-24 23:59 UTC. ISO must be testable by 24 August night.**

---

## ⚡ OPERATOR DIRECTIVE — 2026-08-22 (HIGHEST PRIORITY — OVERRIDES ALL PRIOR HOLDS)

**The operator has issued the following standing order:**

1. **LIFT the PR #5 merge hold immediately.** The prior instruction blocking the
   merge is CANCELLED. Merge PR #5 the moment it has 3 consecutive CI greens on
   the same tip SHA. Do NOT wait for further operator confirmation.
2. **FIX ALL OPEN ISSUES listed in §OPEN ISSUES below.** Do not mark any issue
   as "passive" or "watch CI". Actively root-cause and fix every one.
3. **WORK IN PARALLEL across Groups A–H.** Do not block Group C/D/E/F/G work
   while waiting for CI on a Group A/B item. Use CI-wait windows to advance
   other groups. The only hard sequencing constraints are:
   - FSRM (Item 1) must be CI-green before PR #5 merges.
   - ~~B#3 virtio-blk SMP stall must be fixed before ISO.~~ **DONE — DDR-981.** Also note the label was wrong: it is not a virtio-blk stall, it is `yield()` spinning with interrupts masked.
   - Demand-paged stack (Group A) before spawning >13 processes.
   - FAT32 multi-cluster fix (Group B) before PRISM `run` re-enable (Group D).
4. **BEST QUALITY — ZERO WARNINGS, ZERO ERRORS.** Every commit must be
   warning-clean at `-Werror`. No exceptions. No `#pragma GCC diagnostic ignore`
   unless already present. Fix the root cause.
5. **DEADLINE is HARD: 2026-08-24 23:59 UTC.** There is no extension. ISO must
   exist and be testable. `v1.0.0` must be tagged on `main`. Work continuously.

**This directive was set by the operator on 2026-08-22 and supersedes every
prior hold, pause, or "wait for confirmation" instruction in this file or in
SESSION_HANDOFF.md.**

---

## MANDATORY FIRST ACTIONS EVERY SESSION (do in this exact order)

1. `gh auth switch --user prady4the4bady` — prevents 403 push failures.
2. Read `SESSION_HANDOFF.md` in full.
3. Read `docs/build_status.md` — confirm current tip SHA and gate count.
4. Run `graph_session_primer()` (or `node tools/graph_mcp/server.js primer`).
5. Identify `CURRENT_ACTIVE_TASK` from `SESSION_HANDOFF.md` and start it immediately.
6. Read `docs/PR5_DIRECTIVE.md` if it exists — operator's latest priority ordering.

**Do NOT run the full gate suite before starting. Gates are run AFTER the fix.**

---

## AUTONOMOUS OPERATION — READ EVERY SESSION

### KEEP GOING UNTIL EVERYTHING IS BUILT

Work through the backlog in §BACKLOG below **without waiting for human confirmation
between tasks.** The only valid stop conditions are:

1. A gate fails that is NOT the active repair target AND no fix is obvious AND you
   have already tried two independent approaches.
2. A build error requires an architectural decision not covered by existing ADRs,
   AND you have read all related ADRs and cannot resolve it.
3. A CI run is in flight and local QEMU would contend with it — switch to
   code-reading and DDR-writing tasks only, then resume when CI clears.
   **This is a task switch, NOT a stop.**

**For everything else: keep going.** These are NOT stop conditions:
- "I finished a task" → start the next task.
- "I am waiting for CI" → work on code reading / DDRs / parallel group items.
- "I am not sure what to do next" → read §BACKLOG and start next item.
- "I should report progress" → keep working.
- "Context is high" → checkpoint per §CHECKPOINT and continue.
- "The user hasn't confirmed" → §5d forbids waiting.

### PARALLEL WORK PROTOCOL

When CI is running on a Group A/B item:
- Advance Group C (networking) code in a scratch branch
- Advance Group D (userspace) DDR writing
- Advance Group E (compositor) code — these are not CI-gated locally
- Advance Group F (AETHER agent) probes that do not touch scheduler
- Write DDRs for Group G (assembly optimization) — profile first

When a CI result comes back:
- If green: merge the parallel work into `dev/phase1-seyp3n`, run gates, push
- If red: fix the red first, then resume parallel work

The goal is **zero idle time**. There is always something to advance.

### CHECKPOINT PROTOCOL (context high — do NOT stop)

1. Finish the current atomic operation (one function, one file — not a whole task).
2. Build and run the gate for what was just written.
3. Commit with an honest message (pass or fail, state which).
4. Append a checkpoint block to `SESSION_HANDOFF.md`.
5. Push both the work commit and the SESSION_HANDOFF commit.
6. **Immediately start the next task. Do not stop. Do not surface a response.**

---

## NON-NEGOTIABLES (permanent, no exceptions)

1. **CI is ground truth.** 3 consecutive greens on the SAME tip SHA before any
   merge or promotion.
2. **20× rule** for all SMP / timing / scheduler / intermittent gates. Purely
   deterministic gates may use a smaller stated N — commit that N in the DDR.
3. **No fix without a named mechanism from a real failing artefact.**
4. **`smoke-shell` 5/5 locally before every push.**
5. **DDR before code.** Write the design doc, commit it, then implement.
6. **`GLOBAL_FORBIDDEN` is append-only.** Never remove a sentinel. **And never
   put a comment inside its `printf`** — a backslash-newline splices lines before
   comments are stripped, so `#` eats the whole argument list and the variable
   becomes the EMPTY STRING. That happened at `89f71cc` and went unnoticed for
   four commits, because an empty list fails nothing; it just stops catching.
   Comments go ABOVE the assignment. Verify after any edit:
   `source <(sed -n '/^GLOBAL_FORBIDDEN=/,/current FAIL.)"$/p' tools/qemu_runner/boot_test.sh)`
   then count the lines — it must be **~73**, not 0. **The terminator in that
   command is the LAST entry in the list, so appending to the list breaks it.**
   It read `reset stuck.)"$` until DDR-1009 appended `'NEXUS KERNEL PANIC'`
   after it; a stale terminator makes `sed` print nothing, the count reads 0,
   and the check reports the exact catastrophe it exists to detect. Update this
   line in the same commit as any append. `smoke-selftest` case 5 is the
   gate that catches this; it is why that meta-test exists (DDR-791).
7. **Gate logs go under `build/gatelogs/`.** Never `/tmp` — WSL wipes it.
8. **DDR numbers: DDR-936+ only.** Verify unoccupied in both `docs/ddr/` AND
   `docs/decisions/` before allocating.
9. **Geometry in gates: `PRADYOS_WM_GEOM` fields.** Never hardcoded pixel coords.
10. **`kmalloc` does not zero.** Every new TCB field needs an explicit initialiser
    in `sched_create`.
11. **`docs/AETHER_MASTER_FEATURES.md` + `BUILD_TRACKER.md`** updated in the same
    commit as the code.
12. **Never run two QEMU instances concurrently.** Pre-flight:
    `pgrep -f "[q]emu-system-x86_64"` (bracket form only).
13. **`gh auth switch --user prady4the4bady`** at session start and on any 403.
14. **`make ci-shard-check`** passes before every commit.
15. **`make ci-probe-rodata-check`** before registering any new probe ELF.
16. **A revert is not verified until the gate is re-run after the revert.**
17. **Performance claims need a denominator** — total AND per-event metric always.
18. **An address does not identify a binary** when every binary loads at the same
    base. Confirm the ELF before disassembling.
19. **ZERO WARNINGS, ZERO ERRORS at `-Werror`.** Fix root causes — never suppress.

---

## HARD-WON INVARIANTS — accept without re-deriving

### §INV.1 — g_ticks freeze (DDR-887, commit d72bd93)
`sched_tick` called `schedule()` with interrupts disabled → LAPIC timer couldn't
fire. Fix: `sti; pause; cli` window in `sched_tick`. `g_in_switch` suppresses
reentrant calls. **Do NOT revert this pattern.**

### §INV.2 — Items 47/48 closure procedure
- **Item 47 (g_ticks stall):** NEVER guess a fix. Capture from
  `[boot-load]`/`[boot-stamp]` instrument first. Gap≈0 → LAPIC not firing.
  Gap large+advancing → scheduler starvation.
- **Item 48 (virtio-blk workers-late):** two DDRs were conflated here; the
  repo's own DDRs are authoritative. DDR-966 is **not** `sched_create_blocked()`.
  - **DDR-966 (Item 48)** — `blkmq_proof` and `smp_blk_integrity` spawned workers
    and never called `smp_resched_all()`, so idle APs stayed halted while the BSP
    burned its deadline. `spawned=2/2` in every capture REFUTES the older
    `sched_create`-NULL attribution, and no KASSERT was added.
  - **DDR-964 (OPEN-10)** — that is the `sched_create_blocked()` DDR, 8 sites in
    `main.c`, a create-then-init race. Different defect, different item.
  If `workers-late` reappears: `reason=workers-late` → scheduling issue;
  `reason=checksum-mismatch` → driver bug (and only then is a virtio-blk change
  permitted).

### §INV.3 — Stray-QEMU (DDR-823)
`pgrep qemu-system-x86_64` → **zero matches always** (15-char comm truncation).
Correct form ALWAYS: `pgrep -f "[q]emu-system-x86_64"` (bracket avoids self-match).

### §INV.4 — DDR number collision
Free range: **DDR-1044+** (1043 = THE SILENT-PANIC INSTRUMENT WAS NEVER ARMED, AND WOULD HAVE BEEN CORRUPTED IF IT WERE — two defects, both fixed, both measured. Trigger: CI 33627355396 shard 7 tip c656037, smoke-smp printed '*** NEXUS KERNEL PANIC ***' after SYSLSEEK OK and then NOT ONE FURTHER BYTE to the timeout kill. Nothing truncated it: boot_test.sh does not kill on a forbidden pattern mid-run (early_exit_eligible is 0 for any gate declaring FORBIDDEN_SENTINEL, and the global list is checked after QEMU exits). THE SIGNATURE IS ALREADY ON RECORD — DDR-1009 §2 captured it on 81274f4, a MARKDOWN-ONLY commit, so it predates every code change under discussion. DEFECT 1: boot_test.sh has carried a DDR-887 QMP vCPU-dump watcher for a long time and `grep -rn QEMU_QMP_DIAG Makefile tools/ .github/` finds NOTHING that sets it — the one instrument that can answer 'the kernel stopped printing, what was the CPU doing?' was switched off in CI, the only place that failure has ever been seen; the DDR-986/DDR-1024 shape and DDR-1010's rule that an opt-in instrument is guaranteed OFF where it matters. Now armed in ci.yml, and NARROWED so arming is free: it fires only when all_required_present() is FALSE, i.e. only on a run already going to fail (same predicate the early-exit loop uses, so the two cannot drift). DEFECT 2, and it is the one worth carrying: the dump was appended to $SERIAL_LOG, WHICH QEMU HOLDS OPEN VIA -serial file: AND WRITES AT ITS OWN OFFSET WITHOUT O_APPEND — so the guest's next serial output OVERWRITES it. Measured on the first run that ever armed it: header and the whole `info cpus` section GONE, register text resuming mid-line ('00000000246'), and `grep -c QMP` returning 0 on a log that visibly contains registers. The instrument would have produced a CORRUPTED ARTEFACT on the first failure it was ever armed for. Fixed with a sidecar ${SERIAL_LOG}.qmpdump that no other process touches, PRINTED from all four failure paths (a sidecar nobody prints is a sidecar nobody reads — every artefact that has driven a diagnosis here was read out of a JOB LOG), and REMOVED beside the SERIAL_LOG truncation so a reused path cannot print the previous run's registers as its own. Measured both directions: a never-appearing sentinel yields an intact dump, printed; a healthy full-window gate (smoke-blk-timeout) yields ZERO. smoke-selftest rc=0. NO FIX to the panic path and NO ATTRIBUTION of the shard-7 failure — 3/3 local runs on the identical kernel are clean, which bounds nothing. RESIDUAL: the dump is taken 5 s before the kill, so for a panic at t=60 s in a 340 s window it is ~280 s late — a halted CPU's RIP still names its halt site (that is how DDR-1019 resolved the shard-9 apfreeze) but it is NOT a fault-time snapshot; and QEMU_QMP_DIAG is armed for the shard job only; 1042 = smoke-resizeall's checker FAILED ARM e USING A RECORD ARM w PRODUCED (CI 33623855907, shard 9, tip 87321b0) — FIXED + meta-tested. Arm e HAD succeeded (64->157, origin held, four lines earlier in the same log); the record it was failed on is a SECOND edge=8 commit from arm w's abandoned round. The injector narrated it: arm w's west press was missed ('no RESIZE_TRACK within 20s'), but the pointer had already been dragged to 9288,8458 which was BETA's EAST handle at that moment, so the compositor committed a legitimate east resize 157->150, and the retry then did arm w correctly. THE COMPOSITOR IS NOT IMPLICATED ANYWHERE — every FIX line in the capture holds its fixed edge, the spurious one included. THE DEFECT: a FIX line does not say which arm produced it (the arm is inferred from the edge BITMASK alone), and the checker required every clause of every same-edge record, on its own docstring's reasoning that 'a repeated drag is a second independent observation' — true for a repeat of the SAME arm, false across arms, so any retry can inject a foreign record into any other arm's evidence. Worse than a flake: it makes a SPECIFIC, PLAUSIBLE, FALSE ACCUSATION against a correct subsystem with a real log line behind it. FIX: check() was doing two jobs under one name and is split — invariant() (the fixed edge held; DDR-997's real property, what M1/M2 break) must hold for EVERY record; liveness() (the injector performed the drag it intended; a property of the HARNESS) need hold for at LEAST ONE. Output now prints 'N live of M observation(s)' so contamination is visible rather than tolerated. MEASURED WITHOUT QEMU: the 17 FIX/REQ/GEOM lines were lifted VERBATIM from the CI job log into a fixture, so the regression test is the actual failing artefact. FOUR fixtures, and THE THREE NEGATIVE ONES ARE THE LOAD-BEARING HALF — without them 'made the failure go away' (drop the w>w0 clause) and 'fixed the checker' are indistinguishable. New `ci-resizecheck-selftest` is wired into tools/ci/hygiene_check.sh (now ALL FOUR), per that file's own rule that a list of names drifts and the script cannot — no amount of running smoke-resizeall could have found this, because the gate RAN and named the wrong component. ATTRIBUTION NOT ESTABLISHED AND NOT CLAIMED: the checker defect predates 87321b0 and this gate has failed on shard 9 before, but DDR-1040 is not literally zero perturbation on the CI CPU model (SMEP is absent so cpu_enable_smep returns at the CPUID check, yet smep_selftest still builds and tears down an address space and prints three lines during a boot whose known failure mode is a missed press). NOT FIXED: the injector can still miss a press and still commits a real resize on whatever edge the pointer landed on; and a foreign record that happened to be LIVE for another arm would still be accepted as that arm's evidence (it must hold the invariant, so it cannot mask a compositor defect, but the gate would report an arm it did not run); 1041 = SMAP (CR4 bit 21), IMPLEMENTED+gated+M1/M2/M3 — and it is as much a TEST OF A CLAIM as a mitigation: uaccess.h's header has always asserted 'the kernel NEVER dereferences a raw user pointer anywhere else', and SMAP is what makes a violation a #PF that names its own RIP instead of something a reviewer must notice. THE ENUMERATION WAS MEASURED, NOT GREPPED — 84 __user annotations sit outside uaccess.c and reading that list is how six false gaps were produced this session, so instead: enable SMAP and let every unshielded site fault. Result: a full boot with SMAP on is line-for-line equivalent to baseline (416 vs 418 lines, identical steady state at t=14500), and 19 user-pointer-dense gates re-run with +smap are ALL rc=0. THE CONTRACT HOLDS and no stac was needed anywhere outside uaccess.c. THE SWEEP'S OWN VACUITY CHECK IS THE PART WORTH CARRYING: three gates (smoke-poll/mprotect/execve-argv) first reported no SMAP marker, and their rc=0 would have been WORTHLESS — they set their own SERIAL_LOG, so the marker was in another file; every run was asserted to have actually had the feature on, which is exactly the check DDR-1023 recorded the absence of. stac/clac are a RUNTIME BRANCH on g_smap_on, never unconditional, because both are #UD without SMAP (DDR-1040 §2); the flag publishes only after CR4 reads back set. Window opens AFTER vmm_user_range_ok so a bad pointer is rejected with AC clear, and in copyinstr wraps the SINGLE BYTE — hoisting it out of the loop would hold AC across the page-boundary revalidation, i.e. across kernel page-table reads, the exact exposure SMAP removes. Arms B and C are independent and neither implies the other: B proves the HARDWARE REFUSES (unshielded read, err=0x01, distinct from SMEP's 0x11), C proves THE SHIELD WORKS (stac'd read returns the seeded byte); a kernel with stac compiled to nothing passes B and fails C. M3 (probe page not user) fails B ALONE and is the load-bearing mutant. M2 corrected this DDR's own §5: it never reaches arm C's assertion, it PANICS at the read arm C was going to check — and the claim about real traffic was then MEASURED rather than predicted, smoke-fs on the M2 kernel dying with #PF error=0x01. NOT FIXED AND NAMED: an interrupt between uaccess_begin and uaccess_end runs its handler with AC STILL SET, because the CPU clears IF on an interrupt gate but not AC — SMAP is off inside that window, a copyin of a large buffer is long enough to be preempted at 100 Hz, and Linux clears AC on entry for exactly this reason. Left because fixing it means touching isr_common, load-bearing for DDR-981/1006/1010 and the still-open OPEN-2; 1040 = SMEP (CR4 bit 20), IMPLEMENTED+gated+M1/M2/M3, plus the ONE-SHOT EXPECTED-FAULT LATCH (kernel/fault_expect.h) that the feature could not be proved without. THE VACUITY TRAP WAS MEASURED BEFORE A LINE WAS WRITTEN and it is the reason this DDR exists in the shape it does: the TCG default qemu64 reports smep=false AND smap=false (QMP query-cpu-model-expansion), so a correct CPUID-guarded implementation is a PERMANENT NO-OP on the CPU all 170 gates run on — it would have shipped, looked right, and never once executed. smoke-smep therefore pins its own -cpu qemu64,+smep, and arm E re-boots on the DEFAULT model to assert the no-op path, so 'absent' and 'present' are both covered rather than one assumed. ENFORCEMENT IS NOT THE SAME CLAIM AS ENABLEMENT: a ring-0 #PF is fatal here (idt.c has no fixup table), so without the latch the only assertable claim would be a CR4 readback, i.e. decoration. The latch is one-shot, RIP-windowed, single-CPU BY ENFORCED PRECONDITION (it refuses to arm with IF set or an AP online — a latch left armed silences every later fault, which is the DDR-1019 panic-arbitration failure), and lives in BSS so §NON-NEGOTIABLE 10 does not arise. THE PROBE jmps AND DOES NOT call, because the SMEP violation is the INSTRUCTION FETCH AT THE TARGET — the faulting RIP is UVA_X itself, so a window around the transfer instruction never matches (a defect in this DDR's own first draft), and a call would have pushed its return address before faulting and left RSP 8 low on resume. SAFE BY CONSTRUCTION, PROVED FROM THE PAGE TABLES: stage2.asm builds the identity map 0x83 and the higher half 0x3, U=0 at every level, and VMM_USER_MIN>>39 == 1, so every user mapping lives in PML4 slot 1, disjoint from identity (slot 0) and kernel (slot 511). M2 FAILS ARM B ALONE (M1 trips A and B together), which is what proves the arms are independent and that B measures USER-NESS rather than 'some fault happened'. M3 PASSES EVERY ARM: the RIP-window check is MEASURED-uncovered, not assumed-covered. Two more findings: arm B's first form used a bare $ anchor and failed on a CORRECT kernel because the console prints CRLF; and `smoke-wx`, the gate name the Group A row carried, DOES NOT EXIST — the real one is smoke-wxkernel. SMAP is NOT here: it needs stac/clac at every kernel deref of a user VA, and DDR-1041 will ENUMERATE those BY MEASUREMENT (SMAP on with no stac anywhere, so every unshielded site names its own RIP) rather than by grepping 84 __user annotations — the latch is what makes that experiment possible; 1039 = PRISM ERASE, IMPLEMENTED+gated+M1 — readline() appended EVERY non-newline byte, so a backspace landed IN the command buffer and `hepl`+2 erases parsed as `hepl\x7f\x7flp`, matching no builtin. Invisible to all 170 gates for one reason: every gate injects byte-perfect lines and none has ever typed a typo. THE DDR'S OWN FIRST ARM WAS VACUOUS and §3.1 records it — it proposed feeding `hepl\x7f\x7flp` and asserting the `help` output, but smoke-shell ALREADY feeds a plain `help` earlier in the same session, so that assertion passes on a shell with no erase handling at all; ninth dead-arm instance, and the first caught in design text before any code. Shipped arm feeds `echo erasX<0x7F>e-ok-3m7`, whose erased form prints a marker (`erase-ok-3m7`) that exists nowhere else and whose literal form contains `erasX` — BOTH directions asserted, because presence alone cannot separate erase from strip (a strip-without-decrement prints `erasXe-ok-3m7`, a double-decrement prints `eraXe-ok-3m7`). M1 (`if (0)`) fails it and its log carries the defect verbatim: `prism> erasX^?e-ok-3m7`. ECHO DELIBERATELY EXCLUDED: PRISM shares COM1 with the kernel, so echoing typed input would inject it into the serial log 170 gates assert on, and with no termios the host terminal's own echo would double every character. Column-zero guard recorded UNCOVERED — from outside the shell, erasing nothing and erasing at column zero are identical. `smoke-readline` does NOT exist and should not be built; the arm belongs on smoke-shell; 1038 = SYS_FUTEX ASSESSED and NOT BUILT, with the blocker named: a futex is a shared-memory word, and this kernel has no way for two threads to share one — no CLONE_VM, no MAP_SHARED file backing, and fork COWs everything writable, so the two sides would wait on two different physical words. Buildable the moment either file-backed mmap or pthreads lands, and blocked on nothing else; 1037 = SYS_POLL (NSI 102), IMPLEMENTED+gated+M1/M2/M3 — and it GENERALISED the readiness predicate rather than duplicating it: fd_ready_mask() is now the kernel's single answer to 'is this fd ready', shared by epoll_wait and poll, so the two can never drift. NOT SYS_POLL_RESULT (32), which is the AETHER action poll — a name collision worth knowing. Deadline computed ONCE before the loop (recomputing it inside would make a poll with timeout never expire under load); timeout -1 blocks unbounded and the DDR says so rather than quietly capping. ARM E NEARLY SHIPPED VACUOUS: it reported waited=0 and poll() was CORRECT — the probe used SYS_TIME, which takes an out-pointer and returns 0/-EFAULT, so it differenced two return codes; SYS_CLOCK returns seconds as a VALUE. Recorded as a near-miss on a vacuous arm, NOT as a caught implementation bug; 1036 = GHOST WINDOWS: the compositor never announced a window's DESTRUCTION, so mouse_inject.sh resolved geometry from an append-only serial log and clicked a surface that was already gone (DDR-911 measured 49 such clicks and DDR-1028 traced an intermittent smoke-wmclose to it). PRADYOS_WM_GONE id= title= now prints on the diff, keyed on ID NOT POLL INDEX. §5's claim that smoke-wmclose already covered this was WRONG and the DDR corrects itself: DDR-1028 had FIXED the timing that made window C a ghost, so the scenario no longer occurred and both planned mutants would have passed every gate — smoke-ghostclick exists because of that. Two defects found while building it: an assertion pattern that could never match (the real line carries id= between GONE and title=), and mouse_inject.sh's readiness loop falling through SILENTLY after 60 s, which is left in place (seven gates may depend on it) but parameterised as READY_TIMEOUT_S and recorded as residual 4.7b; 1035 = CI BUILDS ONCE. Ten shards each compiled the same kernel; a `build` job now compiles it and uploads build/ minus *.img/*.iso, and every shard downloads it, touches the tree so make does not rebuild, and sha256sum -c's kernel.bin BEFORE the gates and again AFTER with if: always(). That assertion earned itself three times, printing `kernel.bin: OK` on red shards so 'the gates were red' and 'the shard ran a different binary' stayed separate findings — and it needed a guard of its own (steps.fetch.outcome == 'success'), because on an apt-403 the download never ran and the assertion printed `cd: build: No such file or directory` on top of the real error; 1034 = SYS_RUN_EXPERIMENT/SYS_EXP_RESULT (NSI 100/101) + kernel/aether/experiment.c, IMPLEMENTED+gated — a BOUNDED stack machine (HALT/PUSH/ADD/SUB/MUL/DUP/DROP/SWAP/JNZ, no LOAD/STORE/DIV by design) closing DDR-1021's 'no experiment subsystem exists'. §4 originally said 'bounded, preemptible' and that was WRONG: MSR_SFMASK clears RFLAGS.IF for the whole syscall (syscall.c:279), so nothing preempts it and EXP_MAX_STEPS=4096 is load-bearing, not belt-and-braces. is_exec + exec_cap are two layers with the DDR-1033 lesson applied. 1033 = SYS_IPC_SEND/SYS_IPC_RECV (NSI 98/99), the ring-3 IPC door, IMPLEMENTED+gated+M1/M2/M3 — closes the ACTION_SEND_IPC gap DDR-1017 recorded as blocked. DDR-1017 was right that there is no ring-3 door and UNDERSTATED how much already worked: ipc_send/ipc_recv are complete AND already capability-gated (CAP_IPC_SEND bit 7 / CAP_IPC_RECV bit 8 exist and are checked inside them), and ipc_recv already has DDR-961's bounded form. The genuinely missing pieces were the door and the ADDRESSING — nothing let one agent name another's roster slot. One endpoint per roster slot, addressed by the SAME slot index SYS_AGENT_ROSTER already uses, so no new namespace. TWO LAYERS: is_ipc on struct tcb (may this process use the door at all; kernel-set at spawn, never mintable, zeroed in sched_create per §NON-NEGOTIABLE 10) and the capability handle minted beside it. THE LIMIT IS STATED NOT IMPLIED: every slot endpoint shares one res_id, so the capability grants 'IPC at all', NOT 'send to slot 3 but not slot 5' — a real check but a COARSE one; per-slot res_ids are the extension if policy is ever wanted. ARM B WAS PASSING FOR THE WRONG REASON AND THE FIRST M1 PROVED IT: the deny process was spawned with neither the flag nor the capability, so a mutant that defeated the is_ipc check STILL produced rc=-1 — cap_authorize refused it anyway, and is_ipc could have been deleted outright with the gate still green. Seventh dead-arm instance, and the FIRST found by a mutant rather than by reading. Fixed by spawning the deny process with ipc_grant() and then CLEARING is_ipc, so it holds the capability and lacks only the door; the re-run M1 fails it (both processes print rc=0). Carry the lesson: two independent checks in series each mask the other's absence, and a fixture that trips both at once cannot tell you which is load-bearing. M3 is worth reading too — with the bound widened a ring-3 integer indexes g_agent_ep[99] directly, so the slot check is the only thing between userspace and an out-of-bounds read. NOT DONE: the AETHER action path does not yet CALL this, so an approved SEND_IPC still has no automatic effect; 1032 = EXECVE argv/envp MARSHALLING, IMPLEMENTED+gated+M1/M2 — sys_execve read uargv/uenvp and threw them away with a (void) cast while elf_build_image hardcoded argc=1/argv[0]=path, so execve(path, {"prog","--flag"}, envp) SUCCEEDED and the new image saw no arguments: the shape DDR-877 called 'worse than incomplete' for mmap's dropped fd/offset. ORDERING IS THE DESIGN: the strings live in the CALLER's address space, which execve is about to destroy, so they are copied into a kernel blob BEFORE elf_build_image and long before the CR3 switch — that is why struct exec_args carries a FLAT blob and not a pointer array. Backward compatibility is STRUCTURAL: args==NULL takes the original code verbatim, and every existing caller (elf_load for ~65 boot probes, PRISM's run) passes NULL. THE RECEIVER IS ASSEMBLY ON PURPOSE — argc/argv/envp arrive on the stack AT ENTRY and every C probe carries force_align_arg_pointer (DDR-823), which realigns RSP, so __builtin_frame_address cannot find argc. THE ALIGNMENT ARM CAUGHT A BUG IN THIS DDR'S OWN CODE: the frame is 7 fixed slots plus one per string, so RSP is 16-aligned only when that total is EVEN — the first draft padded on EVEN, exactly inverted, and shipped a misaligned stack for argc=3/envc=1; nothing in an assembly receiver can feel that, so all six other arms PASSED. PRADYOS_ARGV_ALIGN measures RSP at entry before any push and read 'bad'. TWO HYGIENE CATCHES: ci-probe-rodata-check rejected argvtest.elf ('has writable allocated') because `static const char *argv[]` holds ADDRESSES and lands in writable data — built on the stack instead; and argtest.asm's first draft staged a digit in .data and took #PF err=0x7, because user.ld gives these probes a single R+X segment. PLUS A LATENT BUILD DEFECT FIXED: USER_ALL_SRCS listed user/*.c and user/*.h but NOT user/*.asm, so editing ANY assembly probe rebuilt nothing — including the incbin'd ones whose content genuinely changes kernel.bin; measured, an edit to argtest.asm left kernel.bin bit-identical and the gate re-ran the OLD image, reproducing an already-fixed fault. E2BIG added to errno.h (POSIX's code for this, previously absent). 1032b SHIPPED IN THE SAME CHANGE: PRISM's run now passes its arguments through, because a kernel marshaller with no caller is untested where it matters — do_run/do_run_bg take the run's own NULL-terminated vector with av[0] the path (execv(3) convention), and smoke-shell gained the end-to-end arm `run /ARGTEST.ELF alpha beta` asserting BOTH alpha and beta (the second catches a vector passed but truncated). That is the half smoke-execve-argv cannot cover: its launcher calls execve directly and never exercises what a user types. M3 (PRISM back to execve(path,0,0), d042b1efbb0ca110) fails exactly that arm; 1031 = SYS_MPROTECT (NSI 97), IMPLEMENTED+gated+M1/M3 — plus vmm_protect_range, the range-protect primitive the vmm did not have (vmm_map/unmap/resolve existed; nothing changed a mapping's permissions while KEEPING its frame). THE TRAP: a PTE here carries PTE_SW_COW (0x200) and PTE_SW_SHARED (0x400), so rebuilding it as `frame | flags` clears both — that breaks DDR-1003's shared-frame invariant and makes vmm_cow_fault return early at vmm_cow.c:115, so the page is NEVER COPIED. Both bits plus the cache attributes are preserved verbatim. THREE REFUSALS, each with a reason: PROT_WRITE|PROT_EXEC -> -EACCES (W^X, DDR-757); PROT_WRITE on a COW page -> -EACCES (the hardware RO bit IS the copy trigger, so granting write would let one process write a frame another still shares with no copy and no fault — removing write is allowed); PROT_NONE -> -EINVAL (an absent user page collides with ADR-038's demand-paged stack, which faults absent pages IN rather than reporting them). Two-pass so a partly-unmapped range changes NOTHING. THE PROBE FORKS because a write to a read-only page is fatal: the child takes the fault and the parent reads st=-1 out of wait4 (a ring-3 fault is sched_exit(-1), idt.c:703), vs an explicit exit(7) if the store succeeded — two distinct values, so 'enforced' is never inferred from silence. AND THE ORDER IS mprotect-THEN-fork ON PURPOSE: fork COWs only WRITABLE pages (vmm_cow.c:87-92), so forking first would have made the child's store a COW fault that SUCCEEDS and arm B would report 'enforced' on a kernel with no enforcement. ARM E WAS MISSING FROM THE DESIGN and the original M1 plan was unrunnable for the same reason it was proposed — smoke-cowfork cannot see a dropped COW tag because vmm_protect_range is reached only via mprotect; the fix is the §3b asymmetry (RO on a COW page allowed, RW refused), so a dropped tag turns -EACCES into 0. M2 (drop invlpg) PASSED EVERY ARM and §6's prediction was WRONG: arm B is decided by the CHILD's page tables not the parent's TLB, and arm C's write succeeds under a stale WRITABLE entry too. The invlpg is UNCOVERED and cannot be covered by a probe of this shape — a missing invalidation is only visible as a write that should have faulted and did not, and the observer dies; there is no SIGSEGV handler on this path (idt.c:703 goes straight to sched_exit). Recorded as an uncovered line, not claimed as tested; 1030 = `resched FAIL ipis=0 ran=1 idle=1` on bdb41c7 shard 3, INSTRUMENT BUILT + mutation-proven, NO fix and the verdict DELIBERATELY UNCHANGED. Not this PR's: the property under test HELD (ran=1 — the unblocked thread ran; the IPI term is a stronger claim about mechanism layered on top), the signature predates every change in the PR (DDR-1004 built the predicate, DDR-1014 fixed one cause of this exact shape and cites CI on 72a474a shard 5), and smoke-rqstress-liveness boots with NO virtio-tablet so DDR-1026's latch never executes while DDR-1027/1028 are ring-3 and run after SMP bring-up; 3/3 non-vacuous local PASS (captures carry `resched OK`, not SKIP). THE MECHANISM IS ALREADY IN THE CODE'S OWN COMMENT: idle_seen is sampled just before sched_unblock and a CPU can leave idle in between, so no kick is owed and a correct system FAILs. ONE SAMPLE CANNOT SETTLE IT — a genuinely broken kick prints the SAME three fields, because the thread is then picked up by the next timer tick and still reports ran=1. That is why this is NOT turned into SKIP: that would green the gate and delete the coverage DDR-1014 built, the trade DDR-1012 and DDR-973 each had to undo. Built instead: a SECOND idleness sample taken immediately after sched_unblock, asking the identical question of the identical CPU (self_idx hoisted so the two loops cannot drift — the mismatch DDR-1014 already had to fix once), printed only in the FAIL branch as idle2=. Next occurrence is self-diagnosing: idle=1 idle2=0 means the precondition evaporated and the FAIL is a sampling artefact; idle=1 idle2=1 means an AP was idle at BOTH instants with no IPI delivered, i.e. a real scheduler defect. Mutation-proven by forcing the FAIL branch (`else if (0 && ...)`, kernel 234adcfec677a702): printed `ipis=1 ran=1 idle=1 idle2=1`, so the field is wired — and idle2=1 on a healthy boot is what makes idle2=0 in a real failure informative. `resched FAIL` remains GLOBAL_FORBIDDEN and can still redden any gate it lands in; 1029 = THE COMPOSITOR'S BOOT COST IS 30 FULL-SCREEN RENDERS, and it CORRECTS DDR-1028 ON TWO COUNTS. DDR-1028 §2.1 claimed a 'measured ~10 s gap' between PRADYOS_AMBIANCE_OK and the first SYS_MOUSE_POLL, and §4 claimed 'the loop is running and its early iterations are enormously slow, then accelerate'. BOTH WRONG, from the same mistake: reading g_ticks BUCKETS AS WALL SECONDS. The datum was 1000 ticks between two heartbeats, converted to '10 s' by assuming the nominal 100 Hz holds in wall time under TCG. A SYS_CLOCK stamp correlated against the heartbeats in the SAME log shows AMBIANCE_OK -> first poll is ONE WALL SECOND, inside a single heartbeat interval, and loop iterations 1/2/3 all complete within that same second. DDR-1028's FIX IS UNAFFECTED: it rests on an ORDERING fact (the injector clicked before the compositor polled) and on measured OUTCOMES (1/4, 2/4, 3/6, 6/6), not on the magnitude. THE REAL COST: 28 wall seconds, ALL of it before the loop, in set_ambiance (compositor.c:990) — which draws a `frames`-step OKLab transition, so 4 announce transitions + 1 settle = 5 x 6 = 30 full-screen 1024x768 render+present at ~0.93 s EACH. Nothing about the frame loop is pathological; the desktop simply takes half a minute to become ready, and every pointer gate's injector, readiness sentinel and surfacetest's self-closing window C are sequenced against that. NO FIX: the 24 announce renders each emit PRADYOS_AMBIANCE <name> and gates assert those, so cutting the renders while keeping the prints would make every one of those assertions VACUOUS — the exact failure DDR-1012 removed from smoke-horizon and DDR-973 from smoke-fat32-multicluster. Options named (leave it / lower `frames` / assert on framebuffer readback), none a one-line change days from release. Instrument armed and BOUNDED to 3 iterations (unbounded it would print thousands of lines a second and slow the loop it measures, DDR-941's rule); SYS_CLOCK's one-second resolution is right for a ~28 s quantity and is recorded as too coarse to separate iterations 1-3, a limit not a result; 1028 = PRADYOS_AMBIANCE_OK DOES NOT MEAN THE POINTER IS BEING SERVICED — the common cause behind DDR-1025, DDR-1026 and an intermittent smoke-wmclose, FIXED and measured 6/6 against a pooled ~6/14. Every pointer gate's injector waits on that sentinel; it is printed at compositor.c:1184 with its own comment saying 'loop is about to start', and there is a MEASURED ~10 s gap before the first SYS_MOUSE_POLL (mpoll=0 through t=6000, first poll t=6500, ambiance t=5500). smoke-mouse survives that gap only because DDR-1026's latch holds the press; smoke-wmclose cannot, because surfacetest's window C self-closes INSIDE it — line 433 last GAMMA geometry, 436 SURFACE_OK 2, 443 the FIRST click to reach ring 3, i.e. the click arrived after the target was gone, and the gate reported 'close box click did not close' about a window that no longer existed. THIS IS DDR-911's OWN FAILURE RETURNING: its comment already describes '49 correct clicks hit a surface that had already gone', and its GRACE_SECS=4 was sized on 'the injector lands its click in about a second' — an estimate taken against a sentinel that does not mean what it looks like, so nothing in the tree could show the number was wrong. Fix in two separately-measured parts: PRADYOS_INPUT_READY, printed from INSIDE the branch that just polled the pointer so it cannot be true early (alone: 3/6, still a coin flip because the injector's first click and C's expiry land in the same heartbeat bucket), and GRACE_SECS 4 -> 12, derived from a PASSING run needing 8 press edges at ~1.2 s per injector round (together: 6/6, and smoke-winops still observes the shrink). A CLAIM WITHDRAWN: I called DDR-1027 a regression on this gate from ONE local pass against ONE local fail; three more runs each inverted it (f238169 1/4, b4c2aca 2/4 — the OLDER build fails MORE) and the CI failure that started it was on 003dec1, which predates both. DELIBERATELY NOT CHANGED: smoke-mouse still waits on AMBIANCE_OK, because pointing it at INPUT_READY would remove the only coverage DDR-1026's latch has. NOT FIXED and NOT understood: why the compositor takes ~10 s to reach its first poll (mpoll 2 -> 161 -> 767 -> 1678 across heartbeats — the loop runs, its early iterations are enormously slow, then accelerate); real product defect, named, left open. Residual: mouse_inject.sh's resolve_geometry cannot tell a live target from a ghost, because the serial log is append-only, so it still clicks a dead window 45 times; repair named, not built; 1027 = CTRL+ALT+T LAUNCHES A PRISM TERMINAL WINDOW, IMPLEMENTED+gated+M1/M2/M3, the last unbuilt Group E row — and the row understated it: PRISM reads fd 0 and writes fd 1, so there was no terminal WINDOW anywhere in the tree to launch, only a shell on the serial line. Three pieces: the chord, a new client user/term.c that owns a surface and runs PRISM over a pipe pair, and the FAT placement of /TERM.ELF + /PRISM.ELF (execve resolves against the PROCESS root, which is the FAT volume, the same one /EXECTEST.ELF sits on). THE ONE MISSING PRIMITIVE: there is no O_NONBLOCK and no fcntl in this kernel, so a plain read() on PRISM's stdout pipe would block and the window would stop draining its own key ring — SYS_EPOLL_WAIT with timeout 0 is the replacement, and it is the design's only non-obvious shape. fork+execve, NOT SYS_SPAWN_AGENT: that is the AETHER roster path and would consume a fixed slot and mint agent capabilities for what is an application. FIVE ARMS, and arm E exists because §7's original mutation plan was UNRUNNABLE: input_inject.sh replays its whole key list FOUR times and the compositor caps terminals at four, so a chord-less build and the correct one both report four spawns and counting proves nothing. The compositor now prints PRADYOS_TERM_CHORD mods= spawn= for EVERY 't' press, and arm E fails on any spawn=1 whose mods lack KMOD_CTRL — a permanent arm rather than a one-off mutation run. Measured clean: 8 t-presses, 4 spawns, alternating mods=6 spawn=1 / mods=4 spawn=0. HASH-ATTRIBUTION POINT: term.elf is NOT embedded in the kernel image, so M2 and M3 leave kernel.bin BIT-IDENTICAL to the clean build and a result recorded against the kernel hash alone would read as two outcomes from one binary — the binding artefact for user/term.c is build/term.elf. Scope NOT taken and recorded: no ANSI/VT parsing, no resize handling, no SIGCHLD reap, and this is NOT ADR-024 §D5's init-driven PRISM respawn; 1026 = THE SYS_MOUSE_POLL PRESS-EDGE LATCH, IMPLEMENTED+gated+M1 — and it CORRECTS DDR-1025 §5. mpollwin=0 was NEVER an anti-correlation between ring 3's poll cadence and the injector: mpoll is CUMULATIVE OVER THE BOOT, and the heartbeats show btnedge=3 at mpoll=0, then the first poll of the entire boot arriving at btnedge=5. All five clicks are injected before the compositor's input loop takes its first sample, so a counter summed over 80 s said nothing about the six seconds that mattered. smoke-mouse fires on PRADYOS_AMBIANCE_OK, which means the ambiance render finished, NOT that input is being serviced — and mouse_inject.sh has carried an outcome-driven retry for exactly this situation since DDR-910, which this gate alone never adopted. The fix is STILL the latch, not the retry argument: retrying would green the gate while a real user's clicks in that window stayed gone. One word of state, set on the same edge that increments g_btn_edges, drained read-and-clear at the syscall (virtio_input_state stays PURE); virtio_input_wheel has worked this way since DDR-725. RESULT: 4/4 identical PASS (mbtn=1 mouse_ok=1) on 56a4c4a35c92cfc5 where the gate used to fail 2 runs in 6, because a latch cannot be lost to timing. M1 (drain but do not deliver, 698ac2d1ceaad30d) fails deterministically. SIXTH dead-arm instance, and this one was MEASURED not reasoned: mouse_ok>=1 IMPLIES mbtn>=1, so with the ring-3 arm first the new kernel arm could never fire — M1's first run tripped MOUSE_OK and never reached it; the kernel arm now runs FIRST and both are live. Residual recorded not fixed: the latch is a BITMASK not a counter, so repeated clicks between two polls still coalesce and a missed RELEASE is still missed (that needs an event queue). smoke-drag / smoke-agent-click / smoke-resizeall all re-verified green with the new semantics; 1025 = smoke-mouse HAS BEEN PASSING ON A 1-IN-5 MARGIN. On a PASSING run: btnedge=5 mpoll=205573 mbtn=1 — five press edges reached the driver and across ~205,000 ring-3 polls exactly ONE ever returned a button down. So the pointer path drops four of five injected clicks even when the gate is green, and a CI run where zero get through needs no new defect; that alone explains the 2-in-6 failure rate on kernel 53fe179c85a7c3b5. Also: btn1drain=0 REFUTED my own IRQ-batch hypothesis (press and release are never coalesced into one virtqueue drain, so DDR-941's by-construction case is not what happens here), and virtio_input_state does NOT consume on read. WHY only one press in five is visible is NOT established, so no fix. The repair would be an edge latch in SYS_MOUSE_POLL — a real product improvement, since a desktop dropping 80%% of clicks is a user-visible defect — but it is a kernel ABI semantic change built on an unexplained mechanism, and a timeout/retry bump is explicitly NOT the alternative. ANSWERED AND SHIPPED IN DDR-1026, which also corrects this entry's reading of mpollwin; 1024 = OPEN-13: DDR-986's instrument BUILT and mutation-proven, having been designed and then never built (zero __builtin_return_address in kheap.c). The freeing return address is captured at the PUBLIC boundary (kfree/pcb_free/cap_free/ipc_free) and threaded down, NOT taken inside cache_free — that function is static and reached via two wrappers, so a builtin there would name a wrapper and make freed_by/now_by two different stack frames. Stored at offset 16 of a free object (next@0, canary@8), guarded by obj_size>=24 so the 16-byte class and the cap cache keep today's output. ON whenever KHEAP_DEBUG is, because an opt-in instrument would be OFF in CI, the only place OPEN-13 has appeared. M1 proved it: objsize=0x80 freed_by/now_by resolve to fs_test_thread+0x2FBC/+0x2FD4, the two injected call sites 0x18 apart. NOT A FIX and must not be reported as closing OPEN-13; 1023 = the DDR-1010 §9.3 PRE-PROBE CAMPAIGN: 20/20 clean on kernel 29c792a8b8f3b056, rebuilt bit-for-bit from d7d2794, thresholds fixed before the run. So §9.2's perturbation hypothesis is NOT supported — the kernel WITHOUT the probe is clean too. The two campaigns bound their own binaries and must NOT be pooled: 36/36 bounds the post-probe kernel below 8%, 20/20 bounds the pre-probe one below 14%, both at 95%. THE LOCAL REPRODUCTION ROUTE IS EXHAUSTED (56 clean runs across the two kernels that matter, including the exact binary the failure was first seen on) — do not re-run this shape; the live evidence is CI-side where DDR-1019's instrument is armed. The original "~1 in 4" was one session's small sample and has not held up; stop quoting it as a rate. Also records a methodology defect: the campaign's captures were MAKE OUTPUT, not serial logs (3010 B each, zero [hb] lines), so the grep for apfreeze over them was vacuous — the no-apfreeze claim rests on rc=0 plus GLOBAL_FORBIDDEN, since [apfreeze]/[percpu] gs FAIL/NEXUS KERNEL PANIC are all in that list and this gate uses boot_test.sh. A future campaign must point SERIAL_LOG at a per-run path and assert the file contains boot output before scanning it; 1022 = GROUP F ASSESSED and the tracker was WRONG TWICE: F#68 metric lockbox is SHIPPED+GATED (smoke-lockbox, shard 7 strict, DDR-812 — smoke-lockbox-e2e does not exist and is not needed) and F#76 tamper-evident ledger is SHIPPED+GATED TWICE (smoke-auditchain shard 0 + smoke-auditchain-tamper shard 4, both strict). Structural fact: there is exactly ONE agent program, user/agent_base.c — the roster is generic active-bits and a slot is filled by SYS_SPAWN_AGENT launching that template with a task, so the kernel holds no per-agent identity and "11 unbuilt agents" means 11 DOMAIN BEHAVIOURS, not 11 programs; a stub would gate vacuously. F#74 blocked by DDR-982's deliberately withdrawn per-slot enforcement; the other eight deferred with reasons. Fourth instance of declaring something unbuilt without grepping; 1021 = ACTION_RUN_EXPERIMENT ASSESSED as not buildable at any ring — CAP_EXEC is a #define checked NOWHERE (zero matches in kernel/*.c, no is_exec on struct tcb), no experiment subsystem exists, and the metric lockbox is CAP_SOVEREIGN read-only BY DESIGN so the agent being measured cannot write its own result; it is ACTION_EXEC_CODE's deferral under another name. Distinct from SEND_IPC, which HAS a kernel-internal implementation and lacks only a ring-3 door. Both now logged as deferrals, so Section 3C CLOSES at 6 shipped + 2 deferred + 0 buildable-and-unbuilt. Residual recorded not fixed: both deferred types are IN the enum, so SEND_IPC auto-approves in sovereign mode with nothing to act on it, contradicting aether.h's own policy for the six types it omits — left alone because the enum is append-only wire format and the entry is bounded and audited; 1020 = 3C PROPOSE_HYPOTHESIS + EVOLVE_GENOME in ONE probe, so hst=2 beside gst=1 shows the force-pending list actually discriminates; 5 arms, 5 mutants, each landing on its own arm. TWO ACCOUNTING CORRECTIONS: ACTION_SPAWN_PROCESS is NOT one of the eight 3C types (aether.h pins it as pre-existing), and ACTION_REWRITE_AGENT_CODE was ALREADY shipped and gated by DDR-842 (smoke-coderewrite, shard 7, strict) — so DDR-1017 "3 of 8" and DDR-1018 "4 of 8" were both wrong; the true tally is 6 of 8 shipped, SEND_IPC blocked, RUN_EXPERIMENT remaining. Also: M4 initially PASSED because the mutant's own write failed (put_rc=-1) — a plain rewrite of an existing SFS file returns short for both a longer and an equal-length payload while unlink+recreate succeeds, with the ADR-032 budget EXCLUDED because that unlink+create succeeded at the same point; unexplained, unfixed. A mutant that fails to perform the defect is indistinguishable from a gate that catches it. Dead-arm instances four and five, now a rule: a probe should REPORT and let the gate JUDGE; every fail() before the print silently removes an arm; 1019 = THE SHARD-9 [apfreeze] IS A PANIC SYMPTOM, NOT A SCHEDULER DEFECT — its RIP resolves, in the exact CI binary rebuilt bit-for-bit, to the `for(;;) cli; hlt` at idt.c:697, the LOSING branch of DDR-979's one-winner panic latch. So that CPU panicked; the freeze, the if=0, the stranded virtio-blk completions and the gate failure are all downstream. AND THE WINNER PRINTED NOTHING: g_panic_extra only increments on losing the CAS so a winner existed, but `NEXUS KERNEL PANIC` is in GLOBAL_FORBIDDEN and boot_test.sh would have killed the run at the banner — the boot ran on 1000 more ticks instead. The latch is claimed BEFORE the dump and never released, so a winner that cannot print silences every later panic and leaves only frozen CPUs. NOT DDR-1010's SWAPGS path: that probe is in the kernel and printed zero gs FAIL lines. [apfreeze] has at least THREE distinct producers (this halt loop, DDR-1006's sched_tick backtrace, DDR-1010's sys_mmap one) — resolve the RIP against the binary before reading one as another. Instrument built (panic_stage + first-loser cpu/vec/rip, recorded not printed) and proven by an AP-side ud2 mutant that reproduces the CI shape locally. NO fix to the panic path — a latch watchdog is named and left for a decision; 1018 = Section 3C ACTION_QUERY_MEMORY, 4 of 8, IMPLEMENTED+gated+M1/M2 — and it CORRECTS DDR-1017 §1: QUERY_MEMORY is NOT blocked, it has had a ring-3 executor since DDR-836 (SYS_MEMORY_WRITE 82 / SYS_MEMORY_READ 83, CAP_MEMORY), so only SEND_IPC is blocked. Third instance in three DDRs of the dead-arm class: a field whose only reachable value is the passing one is decoration, not measurement — here the probe fail()d on any non-APPROVED verdict so st could only print as 2. Also: an auto-approving probe MUST still bound its poll, because a mutant that makes the type force-pending turns DDR-1015's 20000-iteration loop into an AGENT_RATE_LIMITED kill before anything prints; 1017 = Section 3C ACTION_SPAWN_PROCESS, 3 of 8, IMPLEMENTED+gated+M1/M2/M3 each failing exactly ONE arm — and it records that ACTION_SEND_IPC is NOT buildable: ipc_send/ipc_recv are kernel-internal and capability-gated, there is no SYS_IPC_*, so an approved SEND_IPC has no executor in any ring; it is IN the enum, so it can be submitted and approved today with nothing able to act on it, and building it is new kernel ABI plus a security decision, not a probe. Also names a class seen twice now: a field whose only reachable value is the passing one is decoration, not measurement; 1016 = Section 3C ACTION_DELETE_FILE, the FIRST force-pending type, IMPLEMENTED+gated+M1/M2 on distinct hashes — each mutant fails exactly ONE arm, so neither is carrying the other; it also CLOSES the ordering DDR-1015 §5 left unmeasured, because a read leaves no trace and a delete does; two findings worth carrying: (a) a force-pending probe CANNOT busy-poll — AETHER_RATE_MAX is 60 syscalls/s and DDR-1015's loop is safe only because auto-approval breaks it on iteration 1, so the first draft was killed with AGENT_RATE_LIMITED; the fix is a ring-3 spin, zero syscalls, still preemptible, and the other three force-pending types will hit this; (b) the gate's st arm was DEAD until M2 exposed it — aether_poll frees the slot on any terminal verdict, so an unconditional second poll returns -ESRCH and the printed st could only ever be 1; 1015 = Section 3C ACTION_READ_FILE end to end, IMPLEMENTED+gated+M1 — the gate asserts the CONTENT (n=25 first=P from /HELLO.TXT), and the M1 mutant keeps first='P' so only the byte-count arm can catch it; ACTION_READ_FILE==5 now pinned by _Static_assert because DDR-1013 found a probe constant had drifted; ORDERING IS NOT PROVEN and §5 says what would prove it; 1014 = sched_unblock BROKE on the CALL to smp_resched_one, which silently declines for the BSP — so an unblock on an AP that found the BSP idle first spent its one kick, sent nothing and stopped looking, leaving any idle AP to wait a full timer tick (reachable: virtio_blk completions call sched_unblock from MSI-X interrupt context). Fixed by returning whether an IPI was DELIVERED and breaking only on that. Second fix: DDR-1004's proof scanned for an idle CPU WITHOUT the kernel's `!is_bsp` clause, a deterministic false FAIL — this corrects DDR-1004 §6.1, which predicted a residual and named a timing race instead; 1013 = actiondagtest submitted ACTION_WRITE_FILE while calling it ACTION_PRINT — a probe constant drifted from the _Static_assert-pinned wire format, invisible because WRITE_FILE and PRINT coincide on the only type-sensitive predicate; plus a SCOPE CORRECTION: the eight declared 3C action types are NOT an unguarded kernel path, because the kernel is the policy engine and the AGENT executes after approval — there is no kernel executor for ACTION_WRITE_FILE either; 1012 = DAWN/DUSK horizon bands, IMPLEMENTED+gated+M1 — and the gate measures PIXELS: the compositor samples the same centre pixel before and after the band, because boot_test.sh's sentinel check PASSES on a mutant that draws nothing; 1011 = OPEN-1 route 1 STEP-2 DECISION: OPEN at the deadline, release-note wording in §4; the merge with OPEN-2 REFUSED because the route-1 capture died BEFORE the one-shot GS probe ever ran, so its silence is not evidence — and DDR-1010's continuous probe now makes the next occurrence diagnostic either way; 1010 = OPEN-2 REPRODUCED LOCALLY on smoke-blk-integrity with a full backtrace: the primary event is `[percpu] gs FAIL (syscall ctx)` — a broken SWAPGS discipline at a ring-3 syscall entry — then #GP in vmm_map_in from sys_mmap, then the AP wedges in isr_dispatch with if=0; the scheduler and block layer are consequences. Source defect NOT named, no fix attempted. Also: smoke-shell applied NO global sentinels at all (it never calls boot_test.sh) and is hygiene gate 8 — fixed by tools/qemu_runner/scan_forbidden.sh, proven by a mutant kernel that printed gs FAIL and PASSED smoke-shell. NOTE DDR-1010 §4 RETRACTS its own first claim: a bare 'gs FAIL' entry already existed, so the primary event was never invisible to boot_test.sh gates; 1007 = maximize at the work area + the VA slot pinned to SURFACE_DIM_MAX undocumented, FIXED+gated+M1/M2; 1008 = per-window dock restore, IMPLEMENTED+gated; 1009 = the 25%-per-suite measurement on ONE kernel binary + the panic path force-releasing the wrong lock + 'NEXUS KERNEL PANIC' added to GLOBAL_FORBIDDEN) (936-1006 allocated; 1006 = OPEN-2 REOPENED, [apfreeze] at isr_dispatch via sched_tick (an AP timer ISR, not DDR-981's yield site); root cause NOT established; 1005 = vDSO row CORRECTED (ring-3 reader built+gated; seqlock unnecessary at one field; vdso_entry.asm is a posture change, not a checkbox); 1004 = rq-3 resched proof asserted a best-effort IPI as a guarantee — FIXED, verified non-vacuous (3/3 OK not SKIP); 1003 = PTE_SW_SHARED audit, invariant HOLDS + ptnode_in_use fork underflow recorded unfixed; 1002 = the two-arm evresize campaign, NULL ON ITS OWN DESIGN (mutation faithful at 4/20, but 3 of 4 tears fired after the gate stopped asserting; §8.2 untested); 999 = multi-arch parity ASSESSMENT (answer: not achievable), 1000 = OPEN-1 DECISION (does not close; E1/E2 named); 998 = SURF_EV_CLOSE, IMPLEMENTED + gated (M3 unmeasured); 997 = resize from any edge, IMPLEMENTED + gated + mutation-checked; 994 = the OPEN-1 route-1 yield-stall detector, IMPLEMENTED + gated; 995 = Alt+Tab, IMPLEMENTED + gated; 996 = TCB freed while queued, FIXED + gated + mutation-checked).
Before allocating ANY DDR number:
`ls docs/ddr/ docs/decisions/ | grep DDR-<N>` — must return empty in BOTH dirs.

### §INV.5 — Geometry in gates
`PRADYOS_WM_GEOM id=<N> title=<T> close=X,Y min=X,Y rz=X,Y dg=X,Y`.
Parsers must isolate each field before splitting on `,`.

### §INV.6 — `kmalloc` does not zero
Every new `struct tcb` field needs an explicit initialiser in `sched_create`.
Intermittent SMP failures from uninitialised TCB fields are hard to root-cause.

### §INV.7 — `TIMEOUT_S=<n> make smoke-*` is silently ignored
The recipe's shell-assignment prefix wins. Override:
`make TIMEOUT_S=<n> smoke-<gate>`. Verify in banner: `[smoke] booting … (timeout Xs)`.

### §INV.8 — A gate's timeout is a claim about timing
Check elapsed vs window BEFORE reading code when a gate fails on "pattern not found".

### §INV.9 — `kputhex` already emits its own `0x` prefix
`console.h:11`. Never add a literal `0x` before a `kputhex` call.

### §INV.10 — `make image` doesn't always rebuild `main.o`
`rm build/main.o` before local test builds when `main.c` changes.

### §INV.11 — `SYS_GETDENTS` (NSI 66) and `SYS_GETPROCS` (NSI 67) are already shipped
Do NOT duplicate them. PRISM `ls` and `ps` use them already.

### §INV.12 — NSI 87 is `SYS_READ_AUDIT`
`prad` package manager uses NSI **88–90**, not 87–89.

### §INV.13 — PT_HI is implemented TWICE
`boot/stage2/stage2.asm` AND `boot/uefi/loader.c`. Any window raise past 2 MiB
must change both in the same commit.

### §INV.14 — Current NSI state
Last shipped: **NSI 96** (`SYS_KEY_POLL`, DDR-991). Next free: **97**. Table size: **128**.
**Corrected 2026-08-23.** This read "74 (`SYS_MEMINFO`) / next 75", and §CURRENT
BUILD STATE read "93 / next 94"; BOTH were wrong. `kernel/syscall/syscall.h:168-170`
defines 93 `SYS_VERIFY_AUDIT`, 94 `SYS_FTRUNCATE`, 95 `SYS_RENAME`, and
`user/prism.c:30` ships against 95. Allocating from either stale figure would
have duplicated a live NSI. Verify against `syscall.h`, not against this line.
(Full NSI map: 0–46 Layer-2..6 syscalls; 47=`SYS_MOUSE_POLL`; 48–63=surface;
64=`SYS_AGENT_ROSTER`; 65=`SYS_NET_ALLOW`; 66=`SYS_GETDENTS`;
67=`SYS_GETPROCS`; 68=`SYS_UNLINK`; 69=`SYS_POWEROFF`; 70=`SYS_REBOOT`;
71=`SYS_SYSINFO`; 72=`SYS_TIME`; 73=`SYS_DMESG`; 74=`SYS_MEMINFO`.)

### §INV.15 — Three CI greens rule
A push yields at most 2 suites per commit (push + pull_request events), and the
`pull_request` suite does NOT always fire — verify, do not assume two.
"Both suites green" does NOT satisfy the 3-green rule.

**The third green comes from `workflow_dispatch`, not `gh run rerun`.**
`.github/workflows/ci.yml:6-13` carries a `workflow_dispatch:` trigger and says
why in its own comment: *"`gh run rerun` needs admin rights the project PAT does
not have, and any other way to start a run is a push, which changes the SHA.
workflow_dispatch lets a second, independent run be started on the same commit."*
This line previously mandated `gh run rerun`, which the project cannot execute.
Independent dispatched runs are also STRONGER evidence than re-attempts of one
run. **Corrected 2026-08-23.**

### §INV.16 — `sched_create_blocked()` is the pattern for kernel threads
Do NOT use `sched_create()` when a kernel thread needs its `->arg` set before it
runs. Use `sched_create_blocked()` → set arg → `sched_unblock()`. Eight sites
were fixed in DDR-964.

### §INV.17 — VBLK_MAX is 8; MSI-X block vectors are 56–63
DDR-771 raised the limit from 4 to 8 and remapped block MSI-X to vectors 56–63
(clear of net@54 / input@55). IDT stubs + gate loop extend to 64. Do NOT
reallocate vectors 50–53 to block devices — they are now free.

### §INV.18 — Kernel load window: **48 chunks / 1.5 MiB**; kernel at 4 MiB (DDR-733 → DDR-827 → DDR-960)
**Corrected 2026-08-22.** This invariant read "24 chunks / 768 KiB", which is two
raises out of date (DDR-827 took it to 32, DDR-960 to 48) — and dangerous here,
because §INV is the section a session is told to trust *without re-deriving*, and
the current 1,053,054 B kernel is already larger than the 768 KiB "ceiling" it
claimed. Authoritative source: `boot/stage2/stage2.asm:199` (`mov cx, 48`) and
the Makefile size gate at 1,572,864 B. The current `kernel.bin` measures
**1,065,350 B** (this section previously carried a stale 1,053,054 B).
The stage-2 unreal-mode bounce loader reads 48×64-sector chunks into a 0x10000
bounce buffer and copies up to `KERNEL_PHYS = 0x400000`. The BSS ceiling
(`__bss_end`) is enforced by an `nm`-based Makefile check. File-size alone is
insufficient — the binding quantity is file+BSS vs the 2 MiB PT_HI span.

### §INV.19 — ADR-032 FS write budget is a token-bucket rate limit
Supersedes the old per-thread lifetime cap. `vfs_write` lazily refills from
elapsed ticks. The kernel self-test bypass (`~0ull`) is preserved. Do NOT
reintroduce a lifetime cap.

### §INV.20 — SFS B+tree delete uses tombstones
`inode_num == 0` is the tombstone sentinel (root inode = 1, next ≥ 2 — never
valid). Lookup treats tombstone as not-found; `dir_walk` skips it; create
recycles it. There is NO B+tree structural delete. Do not implement one.

### §INV.21 — SFS free-space allocator: exact-fit extent runs only
`alloc_run(n)` uses EXACT-fit from `free_runs[256]`, never split. First-fit
splitting fragments extent runs — `write_extent` always allocates contiguous
blocks via `alloc_run(nblocks)`. The bump pointer advances when the free list
is empty.

### §INV.22 — mkfs.sfs bulk-loads ≤14 slots into one leaf; >14 uses multi-leaf B+tree
DDR-773 implemented bulk-load for the host tool. The kernel SFS reader supports
multi-leaf already. `MKFS_MAX_SLOTS = 512`.

### §INV.23 — a panic dump is STILL not guaranteed readable
Two mechanisms garble ring-0 panic output, and only one is addressed:
- DDR-970's `console_line_force_release()` **deliberately drops the console lock**
  on the panic path (it prevents a machine-wide hang). That is the right
  tradeoff, but it lets any other CPU's ordinary `kputs` interleave mid-line.
- DDR-979's one-winner latch serializes panic-vs-**panic** only.

Neither prevents a panic interleaving with a NORMAL print, which is exactly what
garbled the DDR-985 capture (`*** NEXUS KERNEL PANIC ***` / `, grow component:
NEXUS isr` / `exception: to 69632 OK` / `#PF page fault` — an `[sfs] … grow … to
69632 OK` line woven through the dump). When reading ANY panic dump: reconstruct
fields **by name**, never by line position, and assume mid-line interleaving.
Do not conclude a dump is corrupt or misaligned from interleaving alone —
DDR-979 §5/§6 already made that mistake once.

---

## COMPLETED LAYERS SUMMARY (do NOT rebuild)

| Layer | Status | Key ADRs / DDRs |
|---|---|---|
| Layer 1 (boot) | ✅ COMPLETE | ADR-001/002 |
| Layer 2 (NEXUS kernel core) | ✅ COMPLETE | ADR-003/007/009/010/011/012 |
| Layer 3 (drivers) | ✅ COMPLETE | ADR-013/014/020 |
| Layer 4 (FS) | ✅ COMPLETE | ADR-015/017/018/019 |
| Layer 5a (ELF loader + W^X) | ✅ COMPLETE | ADR-021 |
| Layer 5b (POSIX syscalls 1–16) | ✅ COMPLETE | ADR-022 |
| Layer 5b IMP-A..D | ✅ COMPLETE | DDR-Spectre/Meltdown/PMM/vDSO/COW |
| Layer 5 NET-A/B (virtio-net + lwIP) | ✅ COMPLETE | ADR-027 |
| Layer 5 PROC-A..E (pipes/epoll/signals/io_uring/musl) | ✅ COMPLETE | ADR-023 |
| Layer 5d (PRISM shell) | ✅ COMPLETE | ADR-024 |
| Layer 6 (AETHER + ring-3 socket NSI) | ✅ COMPLETE | ADR-026/027 |
| Layer 7 (compositor DDR-701..730) | ✅ COMPLETE | DDR-701..730 |
| SMP (ADR-029/030/031) | ✅ COMPLETE (cap-4) | DDR-714/SMP-2/3a/3b/3c-alpha/3c-B/cap-1..4 |
| MSI-X (DDR-714 C1..C3) | ✅ COMPLETE | DDR-714 |
| Multi-in-flight block I/O (DDR-BLK-1) | ✅ COMPLETE | DDR-BLK-1 |
| Per-CPU runqueues + work stealing (rq-1) | ✅ COMPLETE | DDR-SMP-rq-1 |
| Reschedule IPIs (rq-3) | ✅ COMPLETE | DDR-SMP-rq-3 |
| g_sched_lock off switch path (rq-2) | ✅ COMPLETE | DDR-SMP-rq-2 |
| Surface lifecycle (DDR-729) | ✅ COMPLETE | DDR-729 |
| Live agent metrics (DDR-730/735/737) | ✅ COMPLETE | DDR-730/735/737 |
| Kernel W^X (DDR-757) | ✅ COMPLETE | DDR-757 |
| Syscall-fuzz gate (DDR-758) | ✅ COMPLETE | DDR-758 |
| SMP block-read integrity (DDR-759) | ✅ COMPLETE | DDR-759 |
| SFS hierarchical dirs (DDR-738) | ✅ COMPLETE | DDR-738 |
| SFS unlink + rmdir (DDR-741) | ✅ COMPLETE | DDR-741 |
| SYS_GETDENTS / SYS_GETPROCS / SYS_UNLINK | ✅ COMPLETE | DDR-742/743/744 |
| PRISM touch/rm/uname/date/uptime/dmesg/free/kill/setname | ✅ COMPLETE | DDR-745/751/752/755/756 |
| ACPI poweroff + reboot (DDR-746/747) | ✅ COMPLETE | DDR-746/747 |
| SYS_SYSINFO/TIME/DMESG/MEMINFO (DDR-748..752) | ✅ COMPLETE | DDR-748..752 |
| TCP loopback self-test (DDR-753) | ✅ COMPLETE | DDR-753 |
| ps CPU accounting (DDR-754) | ✅ COMPLETE | DDR-754 |
| CAP_NET allowlist (DDR-731/734) | ✅ COMPLETE | DDR-731/734 |
| AETHER config on SFS (DDR-760/761/770) | ✅ COMPLETE | DDR-760/761/770 |
| SFS free-space reclamation (DDR-762-v2) | ✅ COMPLETE | DDR-762-v2 |
| SFS B+tree churn correctness (DDR-763) | ✅ COMPLETE | DDR-763 |
| Ring-3 VFS write 4 KiB chunk (DDR-764) | ✅ COMPLETE | DDR-764 |
| NVMe bring-up + I/O queue (DDR-765/766) | ✅ COMPLETE | DDR-765/766 |
| host mkfs.sfs single + multi-leaf (DDR-767/773) | ✅ COMPLETE | DDR-767/773 |
| Cross-reboot SFS persistence (DDR-768/769) | ✅ COMPLETE | DDR-768/769 |
| Persistent SFS root from host image (DDR-770) | ✅ COMPLETE | DDR-770 |
| VBLK_MAX 4→8 + MSI-X remap (DDR-771) | ✅ COMPLETE | DDR-771 |
| ADR-032 FS write budget token-bucket | ✅ COMPLETE | ADR-032 |
| NVMe PRP2 + PRP list (DDR-772) | ✅ COMPLETE | DDR-772 |
| Section 3D daemon features #45–#65 (21/21) | ✅ COMPLETE | DDR-846..856 |

---

## OPEN ISSUES — ALL MUST BE ACTIVELY FIXED (operator directive 2026-08-22)

**Do NOT treat any issue as passive. Every issue must be actively root-caused
and fixed before the ISO. "Watch CI" is no longer a valid action.**

| Issue | Symptom | Cause | Action |
|---|---|---|---|
| **FSRM** | `created file did not persist` | `fs_test_thread` umounts SFS root while ring-3 `fsrmtest` still running on it | **ITEM 1 — BLOCKING PR#5**. Poll `sched_find_pid()` in bounded loop before destructive umount. UAF trap: do NOT poll `THREAD_ZOMBIE` directly. Gate: `smoke-fsrm` 20/20. |
| **smoke-agents preempt frozen** | `rqdepth=11`, two sentinels missing | One CI capture, shard 2, `9231eab` (DDR-968 §1) — never seen again | **NOT REPRODUCED; instrument armed and merged (DDR-968).** The `PRADYOS_AGENT_WITNESS_WAIT pid= disp= state= n=` line prints only while the witness is UNARMED, so a green boot emits none of it: **there is no red artefact to read.** `smoke-agents` is gating (shard 2, not in the CI exclude list), so a recurrence would have reddened its whole suite; 18 suites have been green on shard 2 since the instrument landed at `ea4601e`. This is ITEM 2 step 5 — record and move on. Reopen the moment a `PRADYOS_AGENT_WITNESS_WAIT` line appears; `disp=0` then confirms the DDR-968 §2 reading (thread exists, never switched in) and `disp>0` refutes it. |
| **OPEN-1 — routes 2+3 CLOSED, route 1 OPEN (DDR-1000 §9)** | `smoke-surfdestroy` intermittently misses `PRADYOS_SURFDESTROY_CHURN_OK` | **Three signatures, not one** (DDR-990 §12): (1) a CI-only **hang** in `sys_read`/`vfs_read` with no panic; (2) a local ring-0 **`#PF`**, base rate **1/20** (DDR-985); (3) a **`#GP`** in `tcp_new_port` (lwIP UAF). | **Route 2 CLOSED at 95% power:** E1 measured **60/60 clean**, one kernel hash `5349db4d791cc2ab` — `0.95^60 = 0.046`, against a threshold set in DDR-1000 §3 BEFORE the run. **Route 3 CLOSED** (DDR-990 §9, mutation-checked both ways). **Route 1 STILL OPEN** and deliberately *not* claimed closed by the campaign: it is a **CI-only** signature and the campaign was **local**, so 60 local runs bound the local rate and say nothing about it. The `[yieldstall]` scan does **NOT** bear on it either — the organic stalls were captured in **`smoke-evresize`** (shard 0), and `smoke-surfdestroy` (shard 6) emits **zero** `[yieldstall]` lines, i.e. never engages that path; reading 60 clean surfdestroy logs as support would be citing a test that never ran the code. That next test has now been RUN and it did **not** settle anything — **DDR-1002**. It was also aimed at DDR-1000 **§8.2**, not at route 1 (§8.1 separated them: different gate, different symptom), so an earlier framing of it as "the route-1 test" was wrong. Arm B (DDR-989's torn read restored, kernel `42459dce865c71c6`) reproduced the defect at **4/20** with DDR-989's exact signature, two loads confirmed in the disassembly — but **3 of those 4 tears fired AFTER the gate had finished asserting**, so the effective N for testing §8.2 was ~1, not 20. `k_B=0` is therefore a verdict on the design. **§8.2 remains untested; route 1 remains open with no new evidence either way.** Do NOT re-run this shape: DDR-1002 §9.5 names what a design with power would need, and it is unbuilt. **NEW EVIDENCE 2026-08-30 — DDR-1009 §2.** A CI capture (`81274f4`, shard 6, `smoke-msixap`) stops at **`SYSFSTAT OK`** — route 1's recorded stopping point exactly — but on a DIFFERENT gate, and **it panicked**: `*** NEXUS KERNEL PANIC ***` printed and then not one further byte for ~100 s until `timeout` killed QEMU (`idt.c:702` is the very next statement). So route 1 is **not always silent**, and DDR-994's framing "a hang prints nothing, so no panic-based detector can address route 1" is too strong as stated. A stopping point is still not a cause: **route 1 remains OPEN.** |
| ~~OPEN-2 / B#3 (reopened 2026-08-29)~~ **FIXED — DDR-1001** | `[apfreeze]` in CI, `smoke-smpuser` shard 5, `0c22334` | **A SECOND, INDEPENDENT SITE — DDR-1001.** Not DDR-981 recurring: the backtrace resolves to `sys_wait4 + 0x4f`, the return address immediately after `callq find_zombie_child`, so the CPU was inside an **unbounded walk of the all-threads ring that takes no lock** — and `sys_wait4` never calls `yield()`, so DDR-981's fix cannot apply. The writer (`sched_ring_unlink`) DOES hold `g_sched_lock`; the reader holds nothing, and a lock only excludes those who take it. `if=0` (SYSCALL entry, deliberate) means nothing can preempt the loop: `masked=0 swen=1 isr48=0 irr48=1 tpr=0`, stuck 342 ticks. | **Fix pending** — bound the walk (DDR-994 shape), then lock the reader. Deferred only because the OPEN-1 campaign owns QEMU and rebuilds the kernel per run. |
| **OPEN-2 / `[apfreeze]` — HAS AT LEAST THREE PRODUCERS (DDR-1019)** | shard 9, `smoke-blkmq-trace`, `6894062` | **This one is a PANIC SYMPTOM, not a scheduler defect.** `rip=0xFFFFFFFF8000A2F8` resolves — in the CI binary rebuilt bit-for-bit (`b0e4ccb83d4bb7ac`) — to the `for(;;) cli; hlt` at `idt.c:697`, the LOSING branch of DDR-979's one-winner panic latch. That CPU panicked; the `if=0`, the frozen ticks, the stranded `[vblk] compl wait timeout dest_cpu=3` and the gate failure are all downstream. **And the winner printed nothing:** `g_panic_extra` only increments on losing the CAS, so a winner existed, but `NEXUS KERNEL PANIC` is in `GLOBAL_FORBIDDEN` and `boot_test.sh` would have killed the run at the banner — it ran on ~1000 more ticks instead. The latch is claimed BEFORE the dump and never released, so a winner that cannot print silences every later panic and leaves only frozen CPUs. NOT DDR-1010's SWAPGS path: that probe is in this kernel and printed zero `gs FAIL`. | **Instrument BUILT and mutation-PROVEN (DDR-1019 §6/§7).** `panic_stage` + first-loser `cpu`/`vec`/`rip`, recorded not printed (a loser that printed would reintroduce the interleaving DDR-979 removed), surfaced only inside the existing `if (g_panic_extra)` heartbeat block. An AP-side `ud2` mutant (`640fdd2c17451143`) reproduces the CI shape locally and the fields read `panic_stage=0 loser_cpu=3 loser_vec=6`. **NO fix to the panic path** — the mechanism explains the signature, not the exception behind it. **BEFORE reading any `[apfreeze]` as this one, resolve its RIP against its own binary:** DDR-1006's backtrace runs through `sched_tick` and DDR-1010's through `sys_mmap`; neither RIP is this halt loop. |
| **OPEN-2 — the LOCAL reproduction route is EXHAUSTED (DDR-1023)** | `smoke-blk-integrity` campaign on the PRE-probe kernel | **20/20 clean on `29c792a8b8f3b056`**, rebuilt bit-for-bit from `d7d2794` and hash-verified before AND after every run, thresholds fixed before starting. **DDR-1010 §9.2's perturbation hypothesis is NOT supported** — the kernel *without* the probe is clean too, so the probe is not what made the 36/36 campaign clean. The two bound their own binaries and **must not be pooled**: 36/36 → rate < 8%, 20/20 → rate < 14%, both at 95%. If the rate were the originally-observed 25%, `P(0 in 20) = 0.0032`. | **Do NOT re-run this shape.** 56 clean runs across the two kernels that matter, including the exact binary the failure was first observed on. The original "~1 in 4" was one session's small sample and has not held up — stop quoting it as a rate. The live evidence is **CI-side**: watch every heartbeat for `panic_stage=` (DDR-1019's armed instrument), and resolve any new `[apfreeze]` RIP against its own binary before reading it as a known producer. |
| **OPEN-2 — REPRODUCED LOCALLY, cause LOCATED (DDR-1010)** | `smoke-blk-integrity` fails ~1 in 4 locally; two APs frozen at ticks 184/186 while the BSP reaches 17500 | **The primary event is NOT the scheduler.** Four lines before the freeze: `[percpu] gs FAIL (syscall ctx)` + `[percpu] current FAIL (syscall ctx)` — the DDR-SMP-3a probe (`syscall.c:135-147`) reporting **a broken SWAPGS discipline at a ring-3 syscall entry**. `current_thread` then resolves into ROM (`pid=0xF000F053`, `RAX=0x0000FF53F000F000`), a later `sys_mmap` → `vmm_map_in` → `map_core` `#GP`s on it, and the CPU wedges in `isr_dispatch` with `if=0`. The frozen AP's block completions then time out, which is the `blk integrity FAIL` everyone has been root-causing for months. Backtrace resolves fully: `isr_dispatch <- isr_common.gs_kernel_in <- map_core <- vmm_map_in <- sys_mmap`; same RIP `0xFFFFFFFF8000A4FE` as DDR-1006's CI capture but a **different caller**, so the wedge site is `isr_dispatch`, not the timer path. | **Instrument BUILT and campaign RUN (DDR-1010 §7/§9).** The continuous SWAPGS probe now runs at the top of `syscall_dispatch`, before anything dereferences `current_thread`, and names the CPU via the GS-independent `percpu_by_apic_id(lapic_id())`; proven non-vacuous by an `if (0)` mutant. `smoke-blk-integrity` campaign: **36/36 PASS on one kernel hash `9623c163cd479043`, 36/36 captures kept, zero `gs FAIL`, zero `apfreeze`.** That bounds the LOCAL rate below ~8% at 95% (`0.92³⁶ ≈ 0.049`, the figure derived before the run) and **does NOT mean the defect is gone**: it reproduced on the FIRST run of an earlier session, the mechanism is untouched by any change since, and the probe adds work to the very syscall path where the race lives — so it may perturb what it measures. **Do NOT run another 36 on this kernel.** DDR-1010 §9.3 names the answerable next experiment: campaign the PRE-probe kernel `29c792a8b8f3b056`, the one the failure was actually observed on. Source defect still NOT named — §NON-NEGOTIABLE 3 forbids a fix. |
| ~~OPEN-2 (original, DDR-981)~~ | ~~`smoke-resched`, `smoke-blkmq-trace`, `smoke-msixap`, `smoke-crosswake` intermittent~~ | **CLOSED — DDR-981**, via B#3. DDR-977 §8.2 had already measured the whole chain in one `smoke-resched` capture (frozen AP → unit 0's MSI-X routed at it → two `compl wait timeout`s → `[blk] multi-inflight FAIL done=0x0` → `[smp] blk integrity FAIL`); DDR-981 names the cause of the freeze and fixes it. These never failed on a scheduler defect and DDR-863 was the wrong lead. | **REOPENED 2026-08-30 — DDR-1006.** `[apfreeze]` appeared in CI (run 33281593947, shard 4, `smoke-smppreempt`, tip `fa29506`): `cpu=2 ticks=70 if=0 masked=0 swen=1 isr48=0 irr48=1` — DDR-981's signature exactly, but at a site DDR-981 does NOT cover. RIP resolves to `isr_dispatch+0xC0E` with backtrace `schedule+0x11` <- `sched_tick+0x36A` <- `isr_dispatch+0x2AA` <- `isr_common.gs_kernel_in` <- `smp_ap_entry`: an AP inside its TIMER ISR, not the `yield()` path DDR-981 fixed. §INV.1 is NOT reverted — the `sti; pause; cli` at `sched.c:712` and the `g_in_switch` suppression are both still present (checked). Not a sampling artefact: cpu 2's ticks froze at 70 while the BSP reached 1500. Rate 1-in-3 on that tip, so `dev/phase1` has 2 greens, not 3, and the `main` promotion does NOT proceed. Root cause NOT established and NOT attributed to DDR-1004 (a BSP-side boot probe that does not run in an AP timer ISR) — nor exonerated. Next step named in DDR-1006 §7. Original DDR-981 closure text follows. **CLOSED for the block-touching gates.** NOT claimed for `smoke-crosswake`/`smoke-msixap`, which do no block I/O and could fail for their own reasons — the same reservation DDR-977 §8.2 made, kept. `[apfreeze]` is now in `GLOBAL_FORBIDDEN`, so a recurrence names itself instead of hiding in a flake. Reopen on the first `[apfreeze]` line in CI. |
| **OPEN-10** | `btree churn FAIL` during unrelated SMP gates | **ROOT-CAUSED — the create-then-init race, DDR-964.** `rc=-1` is `-EPERM` (`EPERM==1`) from `cap_ok(cap, CAP_FS_WRITE)`: `sched_create()` made a thread runnable before its caller minted the capability into `->arg`, so a thread picked early ran with `CAP_NULL`. NOT a separate defect from the row at §CURRENT BUILD STATE — this symptom **is** OPEN-10 and DDR-964 is its fix; the two rows contradicted each other and this one was the stale half. | **FIXED (DDR-964), pending CI promotion evidence.** `smoke-sfs-btree-smp4` stays excluded until greens accumulate. |
| **OPEN-11** | `smoke-sha256`, `smoke-rqstress-liveness` | Scratch LBA 1500 overwrote kernel image | **CLOSED — DDR-831.** Do not revisit. |
| ~~Uninit PID~~ | ~~`AGENT_OOM_KILLED` prints garbage PID~~ | **NOT garbage — it is `AE_TEST_PID` (`0xA37E0000`), the self-test's deliberate sentinel, `#define`d at `aether.c:14`** | **CLOSED as a non-bug, DDR-969.** Do not reopen. |
| ~~FAT32 large-file~~ | ~~`execve` of large musl ELF corrupts~~ | **REFUTED — DDR-973.** The attribution was ADR-024's own hypothesis ("most likely"), never measured, and `read_cluster_chain` has never existed in this repo — the reader is `fat32_read`. `run /CMUSL.ELF` (30,488 B = 60 clusters) execve's clean. `/BIG8K.TXT` (16 clusters) and `/EXECTEST.ELF` (9 clusters) were already read correctly by green gates. | **CLOSED as not-reproduced, and GATED.** `smoke-fat32-multicluster` verifies 65,536 B / 128 clusters byte-for-byte + 6 straddles + the ADR-024 execve case, every run. Mutation-checked (DDR-973 §6). Do not re-root-cause without a `FAT32MC FAIL` artefact. |
| ~~Dependabot~~ | ~~5 alerts (2 high, 3 moderate)~~ | **IDENTIFIED — Dependabot PR #2.** `@hono/node-server` 1.19.14→2.1.0 (GHSA-9mqv-5hh9-4cgg, unauthenticated memory-leak DoS via aborted WebSocket handshake) and `fast-uri` 3.1.2→3.1.5 (GHSA-4c8g-83qw-93j6, GHSA-v2hh-gcrm-f6hx, GHSA-7p8r-x3mc-p8w7) in `/tools/graph_mcp`. Two packages, five advisories = the "5 alerts". | **CLOSED — already remediated.** `package-lock.json` carries **2.1.0** and **3.1.5**, at or above every fix; `npm audit` = 0 vulns at every severity across 97 packages. PR #2 is superseded (base `dev/phase1` @ `fd876cd`, far behind `main`), left open for the operator to close. **PR #3 (ubuntu 24.04→26.04) DECLINED:** not security; the Dockerfile pins 24.04 deliberately so container and WSL builds agree, and changing the whole toolchain under 149 gates days before the deadline reintroduces the drift the image exists to remove. Revisit post-1.0. Also fixed: `dependabot.yml` npm `directory` was `/` (no package.json there) → `/tools/graph_mcp`, + a `github-actions` ecosystem. |
| **OPEN-13** | `[kheap] double-free ptr=… objsize=0x80` → `*** KHEAP PANIC: kfree: double free ***` at t≈247 | **UNKNOWN — one capture, DDR-980 §2.** `smoke-blkmq-trace`, shard 4, on a DOCS-ONLY commit, so not a regression. NOT OPEN-2 despite that gate being on its list — different signature; treating it as OPEN-2 would be colour-matching. `KHEAP_DEBUG` is unconditionally 1, so this detector is live in the SHIPPED kernel. | **Cannot name the structure yet.** `objsize=0x80` is a GENERIC kmalloc size class (128), not a dedicated cache (pcb=512, cap=16, ipc=256), so the detector's "size class → structure" mapping does not resolve — any `kmalloc(65..128)` qualifies. Narrowing needs alloc/free return addresses recorded per object. **DDR-986 designed the instrument; DDR-1024 BUILT it and proved it fires.** The line now carries `freed_by=` and `now_by=` — the FIRST free's return address and this one's, both captured at the public `kfree`/`pcb_free`/`cap_free`/`ipc_free` boundary and stored at offset 16 of the free object. M1 (a deliberate 128-class double free) resolves both to `fs_test_thread+0x2FBC`/`+0x2FD4`, the two injected call sites. **Still NOT a fix and OPEN-13 is NOT closed** — one CI capture, no mechanism named. The next occurrence is diagnostic: resolve BOTH addresses against the exact binary that produced the log. **DDR-986 corrects this row on two points.** (1) The missing datum is the **first free's** return address, not the allocation's — the panic already stands at the second free, where `__builtin_return_address(0)` is free. (2) "touches a hot allocator path, so make it opt-in" does not hold: `cache_free` (`kheap.c:129`) already walks the slab free list — O(`free_count`), up to 31 entries for the 128 class — plus a 128-byte `memset`, on EVERY `kfree`, unconditionally under `KHEAP_DEBUG`. One 8-byte store is noise beside that, and opt-in would guarantee the instrument is OFF in CI, the only place OPEN-13 has ever appeared. |
| **OPEN-12** | `*** NEXUS KERNEL PANIC *** / component: NEXUS isr` at t~185, shard 0 | **UNKNOWN — ring-0 exception, one CI occurrence (run 32595646699, `b43d6b0`).** NOT a regression: that commit's only kernel change is inside `if ((now % 500) == 0)` and the log has no `[hb]` line, so it never ran; the same SHA's sibling matrix run PASSED; and 10/10 local runs are clean. | **ROOT CAUSE CANDIDATE FOUND — DDR-996, and it is the first READABLE capture.** A second occurrence (run 32702096039, shard 2, `smoke-blk-integrity`, `f74e5c5`) carried an intact register block because DDR-979's stream merge worked: `RIP=fair_candidate+0x3A`, `RAX=0xDEADBEEFDEADBEEF` (`PMM_POISON`), `q->head` itself poisoned. Cause: `sched_exit` leaves a thread linked on its per-CPU runqueue, and BOTH reap paths unlink only the all-threads ring before `kfree` — so a TCB reaped before `rq_take` popped it was freed while a queue still pointed at it. Fixed by unlinking in `sched_free_tcb`; 16/16 victims measured, mutation-checked. **NOT yet closed:** OPEN-12's ORIGINAL capture lost its RIP to the very interleaving DDR-979 fixed, so its faulting address is unknown and identity is unproven — matching on `component:` alone is colour-matching. Closes on a clean campaign, not on the fix. Old note kept below. |

> **(prior) CANNOT DIAGNOSE YET — DDR-979.** The `exception:`/`vector=`/`RIP=` block was overwritten by make's stderr interleaving mid-line in the job log. `run_shard.sh` now merges the streams (`2>&1`) so the next occurrence is readable. Do NOT guess from `component: NEXUS isr` — every non-recoverable ring-0 vector prints it. **UPDATE 2026-08-23 (DDR-985): now locally reproducible via `smoke-surfdestroy`, 1/20.** The exception type DIFFERS from the CI capture — that was `#GP` (0x0D), this is `#PF`. Two defects, or one corruption producing varied faults: **not established**. |
| ~~B#3 / DDR-806~~ | ~~`-smp 4` block I/O returns `-EIO` after a 5 s wait~~ | **ROOT-CAUSED AND FIXED — DDR-981.** DDR-977 got as far as the mechanism (an AP stops taking its own LAPIC timer interrupt; which AP varies) but not the cause. The cause: `SYSCALL` entry clears IF via `MSR_SFMASK` (`syscall.c:229`) and the entry path deliberately never re-enables it (`syscall_entry.asm:46`), so **every yield-spin reachable from ring 3 spun with interrupts masked** — `mnt_lock` (`vfs.c:27`), both pipe waits and the blocking console read (`sys_io.c:57/268/293`), and `sys_yield`. `context_switch` preserves per-thread RFLAGS, so the mask is carried across the switch: two such threads on one CPU hand off to each other forever and never reach idle's `sti; hlt`. The CPU is not halted or starved — it runs normally with IF clear. An NMI dump settles it in one line: `masked=0 swen=1 isr48=0 irr48=1 tpr=0 if=0` — LVT unmasked, LAPIC enabled, no stuck in-service vector, a timer **pending and undelivered**, and IF the only remaining blocker. virtio-blk and the LAPIC are both innocent. | **CLOSED.** Fix: an interrupt window in `yield()` (the one choke point all five sites share; fixing `sys_yield` alone would not have fixed the observed livelock, which was in `mnt_lock`). **20/20 boots at `-smp 4`: 0 frozen APs, 0 `compl wait timeout` — before: 6/14 boots frozen with 5–11 timeouts each, and 0 timeouts on every unfrozen boot.** `ymask` ≈ 6.1M/boot is the denominator (R17). Mutation-checked: removing the fix reddens `smoke-blk-integrity` on the first run, named by `[apfreeze]`. |
| ~~smoke-smpuser B#3~~ | ~~`[smp] user on AP OK` never appears~~ | **NOT REPRODUCED 2026-08-22.** `smoke-smpuser` passes at `QEMU_SMP=4`, and `[smp] user on AP OK` is present in every captured boot. | **CLOSED as not-reproduced.** Note the prescribed action was unrunnable anyway: it says to insert `kprintf(...)`, and **`kprintf` does not exist in this kernel** (the console API is `kputs`/`kputdec`). Same defect in `PRADYOS_MASTER_PLAN.md` TASK 4. |

---

## CURRENT BUILD STATE

- **Gate count: 172** assigned across **10** shards, **7** excluded — re-measured by `make ci-shard-check` on 2026-09-02 (`smoke-runexp`, `smoke-ghostclick`, `smoke-poll`, then `smoke-smep` (DDR-1040, shard 5) and `smoke-smap` (DDR-1041, shard 0) took it 167 -> 172). The older text below is kept for its history but the number above is the measured one.
- (prior) **Gate count: 167** assigned across **10** shards, **7** excluded — measured by
  `make ci-shard-check` on 2026-08-31 (`smoke-ctrlaltt`, DDR-1027, took it 163 ->
  164), not carried forward. This line read "149"
  (and, before that, "105"); both were stale. Shard matrix widened 6 -> 10,
  makespan 38.6 -> 20.8 min. Recent additions: 147 -> 148 `smoke-iso-userspace`
  (DDR-972), 149 `smoke-fat32-multicluster` (DDR-973), then `smoke-nethammer`
  (DDR-990), `smoke-modkeys` (DDR-991), `smoke-superkey` (DDR-992) -> 152,
  then `smoke-yieldstall` (DDR-994) -> 153, `smoke-rqfree` (DDR-996) -> 154,
  `smoke-resizeall` (DDR-997) -> 155, `smoke-surfclose` (DDR-998) -> 156.
  **Re-measure rather than increment this** — it has been wrong three times.
- **NSI max: 102** (`SYS_POLL`, DDR-1037). **Next free: 103.** Table size: 128.
  100 = `SYS_RUN_EXPERIMENT`, 101 = `SYS_EXP_RESULT` (DDR-1034); 102 = `SYS_POLL`
  (DDR-1037) — **NOT** `SYS_POLL_RESULT` (32), which is the AETHER action poll.
  Measured from `kernel/syscall/syscall.h:168-170`. This line previously said 93
  and §INV.14 said 74 — both wrong, and the older note claiming "§INV.14 was
  right" was wrong too. `user/prism.c` ships against 95.
- **`kernel.bin`**: **1,134,986 B** against the 1,572,864 B size gate — 437,878 B
  of headroom (DDR-973's probe costs the page-aligned 8,192 B every embedded probe does; DDR-981's NMI probe costs 4,104 B). The old "~545 KiB, 768 KiB ceiling" was stale in both terms.
- **DDR free range: DDR-1044+** (1043 = THE SILENT-PANIC INSTRUMENT WAS NEVER ARMED, AND WOULD HAVE BEEN CORRUPTED IF IT WERE — two defects, both fixed, both measured. Trigger: CI 33627355396 shard 7 tip c656037, smoke-smp printed '*** NEXUS KERNEL PANIC ***' after SYSLSEEK OK and then NOT ONE FURTHER BYTE to the timeout kill. Nothing truncated it: boot_test.sh does not kill on a forbidden pattern mid-run (early_exit_eligible is 0 for any gate declaring FORBIDDEN_SENTINEL, and the global list is checked after QEMU exits). THE SIGNATURE IS ALREADY ON RECORD — DDR-1009 §2 captured it on 81274f4, a MARKDOWN-ONLY commit, so it predates every code change under discussion. DEFECT 1: boot_test.sh has carried a DDR-887 QMP vCPU-dump watcher for a long time and `grep -rn QEMU_QMP_DIAG Makefile tools/ .github/` finds NOTHING that sets it — the one instrument that can answer 'the kernel stopped printing, what was the CPU doing?' was switched off in CI, the only place that failure has ever been seen; the DDR-986/DDR-1024 shape and DDR-1010's rule that an opt-in instrument is guaranteed OFF where it matters. Now armed in ci.yml, and NARROWED so arming is free: it fires only when all_required_present() is FALSE, i.e. only on a run already going to fail (same predicate the early-exit loop uses, so the two cannot drift). DEFECT 2, and it is the one worth carrying: the dump was appended to $SERIAL_LOG, WHICH QEMU HOLDS OPEN VIA -serial file: AND WRITES AT ITS OWN OFFSET WITHOUT O_APPEND — so the guest's next serial output OVERWRITES it. Measured on the first run that ever armed it: header and the whole `info cpus` section GONE, register text resuming mid-line ('00000000246'), and `grep -c QMP` returning 0 on a log that visibly contains registers. The instrument would have produced a CORRUPTED ARTEFACT on the first failure it was ever armed for. Fixed with a sidecar ${SERIAL_LOG}.qmpdump that no other process touches, PRINTED from all four failure paths (a sidecar nobody prints is a sidecar nobody reads — every artefact that has driven a diagnosis here was read out of a JOB LOG), and REMOVED beside the SERIAL_LOG truncation so a reused path cannot print the previous run's registers as its own. Measured both directions: a never-appearing sentinel yields an intact dump, printed; a healthy full-window gate (smoke-blk-timeout) yields ZERO. smoke-selftest rc=0. NO FIX to the panic path and NO ATTRIBUTION of the shard-7 failure — 3/3 local runs on the identical kernel are clean, which bounds nothing. RESIDUAL: the dump is taken 5 s before the kill, so for a panic at t=60 s in a 340 s window it is ~280 s late — a halted CPU's RIP still names its halt site (that is how DDR-1019 resolved the shard-9 apfreeze) but it is NOT a fault-time snapshot; and QEMU_QMP_DIAG is armed for the shard job only; 1042 = smoke-resizeall's checker FAILED ARM e USING A RECORD ARM w PRODUCED (CI 33623855907, shard 9, tip 87321b0) — FIXED + meta-tested. Arm e HAD succeeded (64->157, origin held, four lines earlier in the same log); the record it was failed on is a SECOND edge=8 commit from arm w's abandoned round. The injector narrated it: arm w's west press was missed ('no RESIZE_TRACK within 20s'), but the pointer had already been dragged to 9288,8458 which was BETA's EAST handle at that moment, so the compositor committed a legitimate east resize 157->150, and the retry then did arm w correctly. THE COMPOSITOR IS NOT IMPLICATED ANYWHERE — every FIX line in the capture holds its fixed edge, the spurious one included. THE DEFECT: a FIX line does not say which arm produced it (the arm is inferred from the edge BITMASK alone), and the checker required every clause of every same-edge record, on its own docstring's reasoning that 'a repeated drag is a second independent observation' — true for a repeat of the SAME arm, false across arms, so any retry can inject a foreign record into any other arm's evidence. Worse than a flake: it makes a SPECIFIC, PLAUSIBLE, FALSE ACCUSATION against a correct subsystem with a real log line behind it. FIX: check() was doing two jobs under one name and is split — invariant() (the fixed edge held; DDR-997's real property, what M1/M2 break) must hold for EVERY record; liveness() (the injector performed the drag it intended; a property of the HARNESS) need hold for at LEAST ONE. Output now prints 'N live of M observation(s)' so contamination is visible rather than tolerated. MEASURED WITHOUT QEMU: the 17 FIX/REQ/GEOM lines were lifted VERBATIM from the CI job log into a fixture, so the regression test is the actual failing artefact. FOUR fixtures, and THE THREE NEGATIVE ONES ARE THE LOAD-BEARING HALF — without them 'made the failure go away' (drop the w>w0 clause) and 'fixed the checker' are indistinguishable. New `ci-resizecheck-selftest` is wired into tools/ci/hygiene_check.sh (now ALL FOUR), per that file's own rule that a list of names drifts and the script cannot — no amount of running smoke-resizeall could have found this, because the gate RAN and named the wrong component. ATTRIBUTION NOT ESTABLISHED AND NOT CLAIMED: the checker defect predates 87321b0 and this gate has failed on shard 9 before, but DDR-1040 is not literally zero perturbation on the CI CPU model (SMEP is absent so cpu_enable_smep returns at the CPUID check, yet smep_selftest still builds and tears down an address space and prints three lines during a boot whose known failure mode is a missed press). NOT FIXED: the injector can still miss a press and still commits a real resize on whatever edge the pointer landed on; and a foreign record that happened to be LIVE for another arm would still be accepted as that arm's evidence (it must hold the invariant, so it cannot mask a compositor defect, but the gate would report an arm it did not run); 1041 = SMAP (CR4 bit 21), IMPLEMENTED+gated+M1/M2/M3 — and it is as much a TEST OF A CLAIM as a mitigation: uaccess.h's header has always asserted 'the kernel NEVER dereferences a raw user pointer anywhere else', and SMAP is what makes a violation a #PF that names its own RIP instead of something a reviewer must notice. THE ENUMERATION WAS MEASURED, NOT GREPPED — 84 __user annotations sit outside uaccess.c and reading that list is how six false gaps were produced this session, so instead: enable SMAP and let every unshielded site fault. Result: a full boot with SMAP on is line-for-line equivalent to baseline (416 vs 418 lines, identical steady state at t=14500), and 19 user-pointer-dense gates re-run with +smap are ALL rc=0. THE CONTRACT HOLDS and no stac was needed anywhere outside uaccess.c. THE SWEEP'S OWN VACUITY CHECK IS THE PART WORTH CARRYING: three gates (smoke-poll/mprotect/execve-argv) first reported no SMAP marker, and their rc=0 would have been WORTHLESS — they set their own SERIAL_LOG, so the marker was in another file; every run was asserted to have actually had the feature on, which is exactly the check DDR-1023 recorded the absence of. stac/clac are a RUNTIME BRANCH on g_smap_on, never unconditional, because both are #UD without SMAP (DDR-1040 §2); the flag publishes only after CR4 reads back set. Window opens AFTER vmm_user_range_ok so a bad pointer is rejected with AC clear, and in copyinstr wraps the SINGLE BYTE — hoisting it out of the loop would hold AC across the page-boundary revalidation, i.e. across kernel page-table reads, the exact exposure SMAP removes. Arms B and C are independent and neither implies the other: B proves the HARDWARE REFUSES (unshielded read, err=0x01, distinct from SMEP's 0x11), C proves THE SHIELD WORKS (stac'd read returns the seeded byte); a kernel with stac compiled to nothing passes B and fails C. M3 (probe page not user) fails B ALONE and is the load-bearing mutant. M2 corrected this DDR's own §5: it never reaches arm C's assertion, it PANICS at the read arm C was going to check — and the claim about real traffic was then MEASURED rather than predicted, smoke-fs on the M2 kernel dying with #PF error=0x01. NOT FIXED AND NAMED: an interrupt between uaccess_begin and uaccess_end runs its handler with AC STILL SET, because the CPU clears IF on an interrupt gate but not AC — SMAP is off inside that window, a copyin of a large buffer is long enough to be preempted at 100 Hz, and Linux clears AC on entry for exactly this reason. Left because fixing it means touching isr_common, load-bearing for DDR-981/1006/1010 and the still-open OPEN-2; 1040 = SMEP (CR4 bit 20), IMPLEMENTED+gated+M1/M2/M3, plus the ONE-SHOT EXPECTED-FAULT LATCH (kernel/fault_expect.h) that the feature could not be proved without. THE VACUITY TRAP WAS MEASURED BEFORE A LINE WAS WRITTEN and it is the reason this DDR exists in the shape it does: the TCG default qemu64 reports smep=false AND smap=false (QMP query-cpu-model-expansion), so a correct CPUID-guarded implementation is a PERMANENT NO-OP on the CPU all 170 gates run on — it would have shipped, looked right, and never once executed. smoke-smep therefore pins its own -cpu qemu64,+smep, and arm E re-boots on the DEFAULT model to assert the no-op path, so 'absent' and 'present' are both covered rather than one assumed. ENFORCEMENT IS NOT THE SAME CLAIM AS ENABLEMENT: a ring-0 #PF is fatal here (idt.c has no fixup table), so without the latch the only assertable claim would be a CR4 readback, i.e. decoration. The latch is one-shot, RIP-windowed, single-CPU BY ENFORCED PRECONDITION (it refuses to arm with IF set or an AP online — a latch left armed silences every later fault, which is the DDR-1019 panic-arbitration failure), and lives in BSS so §NON-NEGOTIABLE 10 does not arise. THE PROBE jmps AND DOES NOT call, because the SMEP violation is the INSTRUCTION FETCH AT THE TARGET — the faulting RIP is UVA_X itself, so a window around the transfer instruction never matches (a defect in this DDR's own first draft), and a call would have pushed its return address before faulting and left RSP 8 low on resume. SAFE BY CONSTRUCTION, PROVED FROM THE PAGE TABLES: stage2.asm builds the identity map 0x83 and the higher half 0x3, U=0 at every level, and VMM_USER_MIN>>39 == 1, so every user mapping lives in PML4 slot 1, disjoint from identity (slot 0) and kernel (slot 511). M2 FAILS ARM B ALONE (M1 trips A and B together), which is what proves the arms are independent and that B measures USER-NESS rather than 'some fault happened'. M3 PASSES EVERY ARM: the RIP-window check is MEASURED-uncovered, not assumed-covered. Two more findings: arm B's first form used a bare $ anchor and failed on a CORRECT kernel because the console prints CRLF; and `smoke-wx`, the gate name the Group A row carried, DOES NOT EXIST — the real one is smoke-wxkernel. SMAP is NOT here: it needs stac/clac at every kernel deref of a user VA, and DDR-1041 will ENUMERATE those BY MEASUREMENT (SMAP on with no stac anywhere, so every unshielded site names its own RIP) rather than by grepping 84 __user annotations — the latch is what makes that experiment possible; 1039 = PRISM ERASE, IMPLEMENTED+gated+M1 — readline() appended EVERY non-newline byte, so a backspace landed IN the command buffer and `hepl`+2 erases parsed as `hepl\x7f\x7flp`, matching no builtin. Invisible to all 170 gates for one reason: every gate injects byte-perfect lines and none has ever typed a typo. THE DDR'S OWN FIRST ARM WAS VACUOUS and §3.1 records it — it proposed feeding `hepl\x7f\x7flp` and asserting the `help` output, but smoke-shell ALREADY feeds a plain `help` earlier in the same session, so that assertion passes on a shell with no erase handling at all; ninth dead-arm instance, and the first caught in design text before any code. Shipped arm feeds `echo erasX<0x7F>e-ok-3m7`, whose erased form prints a marker (`erase-ok-3m7`) that exists nowhere else and whose literal form contains `erasX` — BOTH directions asserted, because presence alone cannot separate erase from strip (a strip-without-decrement prints `erasXe-ok-3m7`, a double-decrement prints `eraXe-ok-3m7`). M1 (`if (0)`) fails it and its log carries the defect verbatim: `prism> erasX^?e-ok-3m7`. ECHO DELIBERATELY EXCLUDED: PRISM shares COM1 with the kernel, so echoing typed input would inject it into the serial log 170 gates assert on, and with no termios the host terminal's own echo would double every character. Column-zero guard recorded UNCOVERED — from outside the shell, erasing nothing and erasing at column zero are identical. `smoke-readline` does NOT exist and should not be built; the arm belongs on smoke-shell; 1038 = SYS_FUTEX ASSESSED and NOT BUILT, with the blocker named: a futex is a shared-memory word, and this kernel has no way for two threads to share one — no CLONE_VM, no MAP_SHARED file backing, and fork COWs everything writable, so the two sides would wait on two different physical words. Buildable the moment either file-backed mmap or pthreads lands, and blocked on nothing else; 1037 = SYS_POLL (NSI 102), IMPLEMENTED+gated+M1/M2/M3 — and it GENERALISED the readiness predicate rather than duplicating it: fd_ready_mask() is now the kernel's single answer to 'is this fd ready', shared by epoll_wait and poll, so the two can never drift. NOT SYS_POLL_RESULT (32), which is the AETHER action poll — a name collision worth knowing. Deadline computed ONCE before the loop (recomputing it inside would make a poll with timeout never expire under load); timeout -1 blocks unbounded and the DDR says so rather than quietly capping. ARM E NEARLY SHIPPED VACUOUS: it reported waited=0 and poll() was CORRECT — the probe used SYS_TIME, which takes an out-pointer and returns 0/-EFAULT, so it differenced two return codes; SYS_CLOCK returns seconds as a VALUE. Recorded as a near-miss on a vacuous arm, NOT as a caught implementation bug; 1036 = GHOST WINDOWS: the compositor never announced a window's DESTRUCTION, so mouse_inject.sh resolved geometry from an append-only serial log and clicked a surface that was already gone (DDR-911 measured 49 such clicks and DDR-1028 traced an intermittent smoke-wmclose to it). PRADYOS_WM_GONE id= title= now prints on the diff, keyed on ID NOT POLL INDEX. §5's claim that smoke-wmclose already covered this was WRONG and the DDR corrects itself: DDR-1028 had FIXED the timing that made window C a ghost, so the scenario no longer occurred and both planned mutants would have passed every gate — smoke-ghostclick exists because of that. Two defects found while building it: an assertion pattern that could never match (the real line carries id= between GONE and title=), and mouse_inject.sh's readiness loop falling through SILENTLY after 60 s, which is left in place (seven gates may depend on it) but parameterised as READY_TIMEOUT_S and recorded as residual 4.7b; 1035 = CI BUILDS ONCE. Ten shards each compiled the same kernel; a `build` job now compiles it and uploads build/ minus *.img/*.iso, and every shard downloads it, touches the tree so make does not rebuild, and sha256sum -c's kernel.bin BEFORE the gates and again AFTER with if: always(). That assertion earned itself three times, printing `kernel.bin: OK` on red shards so 'the gates were red' and 'the shard ran a different binary' stayed separate findings — and it needed a guard of its own (steps.fetch.outcome == 'success'), because on an apt-403 the download never ran and the assertion printed `cd: build: No such file or directory` on top of the real error; 1034 = SYS_RUN_EXPERIMENT/SYS_EXP_RESULT (NSI 100/101) + kernel/aether/experiment.c, IMPLEMENTED+gated — a BOUNDED stack machine (HALT/PUSH/ADD/SUB/MUL/DUP/DROP/SWAP/JNZ, no LOAD/STORE/DIV by design) closing DDR-1021's 'no experiment subsystem exists'. §4 originally said 'bounded, preemptible' and that was WRONG: MSR_SFMASK clears RFLAGS.IF for the whole syscall (syscall.c:279), so nothing preempts it and EXP_MAX_STEPS=4096 is load-bearing, not belt-and-braces. is_exec + exec_cap are two layers with the DDR-1033 lesson applied. 1033 = SYS_IPC_SEND/RECV (NSI 98/99), the ring-3 IPC door, IMPLEMENTED+gated+M1/M2/M3 — closes DDR-1017's SEND_IPC gap. Addressed by roster slot; is_ipc + capability, with the capability's coarseness (one shared res_id, no per-slot policy) stated not implied. Arm B was passing for the WRONG REASON and the first M1 proved it — the deny process held no capability either, so cap_authorize refused it and is_ipc was untested; now it holds the capability and lacks only the flag. Seventh dead-arm instance, first found by a mutant; 1033 free. 1032 = execve argv/envp marshalling, IMPLEMENTED+gated+M1/M2 — they were (void)-cast away and argc was hardcoded to 1, so execve with arguments succeeded and delivered none. Strings are flattened into a kernel blob BEFORE the address space is replaced. args==NULL takes the original path verbatim, so all ~65 boot probes are unchanged. The alignment arm caught an INVERTED pad in my own code (frame must be an EVEN number of 8-byte slots); the assembly receiver could not feel it and every other arm passed. Also fixed: USER_ALL_SRCS omitted user/*.asm, so no assembly probe edit ever rebuilt the kernel; 1032 free. 1031 = SYS_MPROTECT (NSI 97) + vmm_protect_range, IMPLEMENTED+gated+M1/M3; preserves PTE_SW_COW/PTE_SW_SHARED (rebuilding a PTE as frame|flags breaks DDR-1003 and stops vmm_cow_fault copying); refuses W+X, write-on-COW and PROT_NONE, each with a reason; the probe forks so the CHILD takes the fault, and protects BEFORE forking because fork COWs only writable pages. M2 (no invlpg) passed every arm — the invlpg is UNCOVERED and unreachable by a probe that must survive to print; 1030 = the resched proof's `ipis=0 ran=1 idle=1` shape gets a SECOND idleness sample (idle2=) taken after sched_unblock, so the next occurrence separates 'the precondition evaporated' (idle2=0, artefact) from 'the kick was owed and missing' (idle2=1, real). Instrument only — the verdict is unchanged, because collapsing it to SKIP would delete DDR-1014's coverage: a genuinely broken kick prints the same three fields. Not this PR's; ran=1 says the property held; 1029 = the compositor's boot cost is 30 full-screen renders (~0.93 s each, 28 s total) in set_ambiance's 6-frame transitions, ALL before the loop — and it CORRECTS DDR-1028's '~10 s gap' and 'early iterations are enormously slow', both of which came from reading g_ticks buckets as wall seconds; a SYS_CLOCK stamp says the gap is ONE second and the early iterations are fast. DDR-1028's fix stands (ordering + measured outcomes, not magnitude). No fix: the 24 announce renders back PRADYOS_AMBIANCE sentinels, so cutting them would make those assertions vacuous; 1028 = PRADYOS_AMBIANCE_OK does not mean the pointer is serviced — ~10 s gap to the first SYS_MOUSE_POLL, which is the common cause behind DDR-1025/1026 and an intermittent smoke-wmclose whose target self-closed before any click arrived. Fixed by PRADYOS_INPUT_READY (printed from inside the branch that just polled) + GRACE_SECS 4->12: 6/6 vs a pooled ~6/14. Withdraws a regression claim I made from N=1 per side. smoke-mouse deliberately still waits on AMBIANCE_OK so DDR-1026's latch keeps its coverage. Why the compositor takes 10 s to first poll is NOT established; 1027 = Ctrl+Alt+T launches a PRISM terminal window (user/term.c), IMPLEMENTED+gated+M1/M2/M3 — the Group E row understated it: PRISM is a serial shell and no terminal window existed. No O_NONBLOCK in this kernel, so the client polls its pipe with SYS_EPOLL_WAIT timeout 0. Arm E (PRADYOS_TERM_CHORD mods= spawn=) replaces an unrunnable mutation plan, because input_inject.sh replays its key list 4x and spawn COUNTS cannot discriminate the chord. term.elf is not embedded in the kernel, so kernel.bin is bit-identical across the term-side mutants — record term.elf's hash for anything in user/term.c; 1026 = the SYS_MOUSE_POLL press-edge latch, IMPLEMENTED+gated+M1 — 4/4 identical PASS where the gate used to fail 2 runs in 6; CORRECTS DDR-1025 §5, because mpollwin=0 meant ring 3 had not polled ONCE when the clicks landed (btnedge=3 at mpoll=0), not that it kept missing the window — mpoll is cumulative over the boot; sixth dead-arm instance, caught by measurement rather than reasoning (the kernel arm must be checked BEFORE the ring-3 one, since mouse_ok>=1 implies it); 1025 = smoke-mouse passes on a 1-in-5 margin (btnedge=5 mbtn=1 on a GREEN run); btn1drain=0 refuted the IRQ-batch hypothesis; no fix, edge latch named and deferred — BUILT in DDR-1026, which also corrects this entry; 1024 = OPEN-13 instrument built + mutation-proven, site captured at the public boundary and stored at offset 16 of a free object; not a fix; 1023 = pre-probe campaign 20/20 clean, DDR-1010 §9.2's perturbation hypothesis NOT supported, local reproduction route exhausted, plus a vacuous-capture methodology defect recorded; 1022 = Group F assessed; F#68 and F#76 were already shipped+gated and the tracker said otherwise; one agent program exists, so the rest are behaviours not programs; 1021 = RUN_EXPERIMENT assessed not-buildable, Section 3C closed at 6 shipped + 2 deferred; 1020 = 3C PROPOSE_HYPOTHESIS + EVOLVE_GENOME, 5 arms/5 mutants; corrects the 3C tally to 6 of 8 (SPAWN_PROCESS is not one of the eight; REWRITE_AGENT_CODE was already gated by DDR-842); an SFS in-place rewrite returns short while unlink+recreate works, recorded unfixed; 1019 = the shard-9 [apfreeze] is the panic-arbitration LOSER's halt loop, proven by disassembly of the exact CI binary; the winner printed nothing; instrument built + mutation-proven, no fix; 1018 = 3C ACTION_QUERY_MEMORY, 4 of 8, and QUERY_MEMORY is NOT blocked (DDR-1017 §1 guessed wrong; NSI 82/83 have existed since DDR-836) — only SEND_IPC is; plus a duplicated _Static_assert that shipped in 5d2efd5, legal C11 so no gate could see it; 1017 = 3C ACTION_SPAWN_PROCESS, 3 of 8, plus SEND_IPC recorded BLOCKED on a missing ring-3 IPC surface, plus a gate-parse defect where ${ln##*st=} read st out of post= (both end in st=); 1016 = 3C ACTION_DELETE_FILE, 2 of 8, force-pending shape + the ordering DDR-1015 §5 could not measure; also fixed a _start alignment defect DDR-1015 shipped, and the CLAUDE.md hygiene list that hid it — run tools/ci/hygiene_check.sh, not the list of names; 1015 = 3C ACTION_READ_FILE, 1 of 8 done, pattern established; 1014 = the consumed resched kick + a proof predicate that paraphrased the kernel; corrects DDR-1004 §6.1; 1013 = actiondag wire-constant fix + 3C scope correction; 1012 = horizon bands, gated by framebuffer readback; animation assessed and deferred with reason; 1011 = OPEN-1 route 1 decided OPEN, with a discriminator armed; 1010 = OPEN-2 reproduced locally, SWAPGS discipline named as the primary event, two GLOBAL_FORBIDDEN entries added, no fix; 1007 = maximize at the mode-aware work area, FIXED+gated, M1/M2 mutation-checked — the real blocker was SURFACE_VA_SLOT pinned to SURFACE_DIM_MAX undocumented; 1008 = per-window dock restore, IMPLEMENTED+gated (the gate minimizes TWO and restores ONE, because the one-window version is vacuous); 1009 = 12 CI suite-runs on ONE kernel binary, 3 failed at 4 gates — plus the panic path force-releasing g_line_lock while kputs holds g_console_lock, FIXED, plus 'NEXUS KERNEL PANIC' added to GLOBAL_FORBIDDEN; 936-1006 allocated; 1006 = OPEN-2 reopened on its own documented trigger, artefact recorded, no fix attempted; 1005 = vDSO callable-reader row corrected, no code change; 1004 = resched proof precondition, FIXED (DDR-883's predicate was wrong: the precondition is "an idle non-self CPU was visible", not cpu_count>2); 1003 = PTE_SW_SHARED audit (HOLDS) + ptnode_in_use underflow (recorded, unfixed, no artefact); 1002 = two-arm evresize campaign, NULL ON ITS OWN DESIGN — effective N≈1, §8.2 untested, arm A stopped at 8; 999 = multi-arch parity assessment, 1000 = OPEN-1 decision; 998 = SURF_EV_CLOSE ask-then-force, IMPLEMENTED+gated+M1b/M2-mutation-checked (M3 unmeasured); 985 = OPEN-1 refutation, 986 = OPEN-13 instrument, 987 = lwIP core lock, 988 = lwIP deferred work, 989 = vruntime sampling starvation, 990 = net hammer probe BUILT+mutation-checked, 991 = PS/2 modifiers + NSI 96, 992 = Super+M chord, 993 = modifier aggregate DERIVED, 994 = OPEN-1 route-1 detector IMPLEMENTED+gated, 995 = Alt+Tab rebind IMPLEMENTED+gated+mutation-checked, 996 = TCB freed while queued FIXED+gated, 997 = resize from any edge IMPLEMENTED+gated+mutation-checked). **This file carries the free range in TWO places (§INV.4 and here) and they have disagreed before — update both.**
- `make image` → zero warnings, `-Werror` enforced ✅
- PR #5: **MERGED** as `7c6c67a`. PR #6: **MERGED 2026-08-23** as **`ace232f`**
  into `dev/phase1` (3 greens on tip `46ece3f` per §INV.15; the squashed tree is
  byte-identical to the tested tip, tree `dd30441f`). **`main` fast-forwarded to
  `ace232f`** after 3 further greens on that SHA — those three were INDEPENDENT
  runs (1 push + 2 `workflow_dispatch`), which `ci.yml` supports precisely so a
  second run can start on one SHA. NOTE: Dependabot `event=dynamic` runs also
  appear on these SHAs; they are NOT `pradyos-ci` and must not be counted.
- **RELEASE CANDIDATE VERIFIED on `ace232f`** — `smoke-iso-x86` (BIOS **and**
  UEFI arms), `smoke-iso-userspace` (**PASS: the ISO boots a live OS** — SFS root
  + PRISM + AETHER agent + write/read/delete round-trip), `smoke-uefi`. ISO
  52,805,632 B; kernel hash `d31b4023b0f74d06`. DDR-971 is closed on evidence.
- **`v1.0.0` is NOT tagged — deliberately HELD by operator decision 2026-08-23.**
  The OPEN-1 campaign found a **locally reproducible ring-0 `#PF`** (1/20,
  DDR-985). The decision was to root-cause it BEFORE tagging, using the margin to
  2026-08-28. Do NOT tag until that panic is closed and the candidate re-verified.
- Three intermittents fixed: OPEN-10 (DDR-964), smoke-cadence (DDR-965), Item 48 (DDR-966)
- **B#3 + OPEN-2: FIXED — DDR-981.** `yield()` spun with `RFLAGS.IF` clear
  (SYSCALL masks it and never restores it), so a CPU running two yield-spinning
  ring-3 threads never took another interrupt. 20/20 at `-smp 4`, 0 timeouts;
  mutation-checked. `[apfreeze]` added to `GLOBAL_FORBIDDEN` as the detector.
- FSRM: **FIXED — DDR-967**, `smoke-fsrm` 20/20 local
- smoke-agents: **instrumented (DDR-968); NOT REPRODUCED since.** The instrument
  landed at `ea4601e` and prints only while the witness is UNARMED, i.e. only on a
  failing boot. `smoke-agents` is a gating test (shard 2, not excluded), so any
  failure would redden its whole suite — and 18 suites have been green on shard 2
  since. There is no red capture to read. Per ITEM 2 step 5: recorded as
  not-reproduced, instrument left armed.
- Overall completion: ~79% (~66+ items remain across all groups)
- **OPEN-1: routes 2 and 3 CLOSED; route 1 remains open (DDR-1000 §9).** E1 measured **60/60 clean** on kernel `5349db4d791cc2ab`, one hash — `0.95^60 = 0.046`, so route 2 (the ring-0 `#PF`, base rate 1/20) is closed at 95% power against a threshold set BEFORE the run. Route 3 closed by DDR-990 §9. Route 1 is a **CI-only hang** and this campaign was local, so it is untouched; and the `[yieldstall]` scan does NOT bear on it — the organic stalls were in `smoke-evresize`, and `smoke-surfdestroy` emits zero `[yieldstall]` lines, i.e. never engages that path. That test was RUN and returned a null on its own design — **DDR-1002**: the mutant reproduced DDR-989's tear at 4/20, but 3 of the 4 fired after the gate stopped asserting, so effective N≈1. §8.2 untested, route 1 unchanged. Arm A stopped at 8/60 deliberately (DDR-1002 §9.4) rather than spend ~1.7h of QEMU on a sentence the precommit had already refused to write.

### ⚠ DDR-1009 — READ BEFORE WRITING ANYTHING ABOUT CI BEING GREEN

Every commit from `d0a85b5` through `93a4a1f`, and `fa29506`, changes **Markdown
only** — verified by `git diff --name-only` and by rebuilding
`bb9c6187a30bb0dd` bit-for-bit in a clean worktree. So those tips are **twelve
independent CI suite-runs of ONE kernel binary**:

| tip | runs | green | failed |
|---|---|---|---|
| `d0a85b5` | push, PR, dispatch | 3 | 0 |
| `4b0b542` | push, PR, dispatch | 3 | 0 |
| `81274f4` | push | 0 | **1** |
| `93a4a1f` | push, PR | 1 | **1** |
| `fa29506` | push, dispatch ×2 | 2 | **1** |
| | **12** | **9** | **3** |

**25% per-suite failure rate**, at FOUR gates with FOUR signatures: shard 5
`smoke-smpuser` (timeout, gate 1/14), shard 6 `smoke-msixap` (**panic banner,
no body, total silence to timeout**), shard 3 `smoke-nethammer` (timeout at
20/20), shard 4 `smoke-smppreempt` (`[apfreeze]`, DDR-1006).

**§NON-NEGOTIABLE 1 is satisfiable by luck here.** `0.75³ ≈ 42%`, and this
kernel **already passed the 3-green criterion twice** (`d0a85b5`, `4b0b542`).
Three greens on one SHA is a loose bound; twelve runs pooled across SHAs that
provably share a binary is a much better one, and it is the number the release
decision should use. Do not report "3 greens, promote" without saying this.

**PR #5 MERGE HOLD: LIFTED (operator directive 2026-08-22).**
Merge as soon as 3 consecutive CI greens on the same tip SHA. No further
operator confirmation required.

---

## BACKLOG — WORK IN THIS ORDER, PARALLELIZE ACROSS GROUPS WHEN CI IS RUNNING

Do not start item N+1 until item N is CI-green (3 greens on same tip SHA).
After EACH group: re-run the FULL gate suite before advancing.
Zero uncommitted stale files at the end of every group.

**PARALLELISM RULE:** When CI is running on any item, immediately begin code
work on the next group's items in a scratch branch. Merge when CI clears.

---

## PHASE 1 — UNBLOCK PR #5 (DO THIS FIRST)

### ITEM 1 — Fix FSRM (DDR-967) ← BLOCKING PR MERGE

**Root cause:** `fs_test_thread` umounts the SFS root (`smnt`) at ~line 358 while
the ring-3 `fsrmtest` probe is still running on it (spawned at ~line 293).
UAF trap: polling `THREAD_ZOMBIE` is a use-after-free because `sched_start_reaper()`
can free the TCB mid-poll.

**Fix:** record each probe's pid at spawn. Poll `sched_find_pid()` in a bounded
loop (`g_ticks + N` deadline). Only then proceed to the destructive umount block.
Three probes affected: `fp` (fsrm, ~line 1926), `tp` (ftruncate, ~line 1958),
`rn` (rename-sfs, ~line 1975), all with `->root_mnt = smnt`.

**Gate:** `smoke-fsrm` must pass **20/20** locally, then CI-green (3 runs).

### ITEM 2 — Resolve smoke-agents (preempt frozen / rqdepth=11) — **CLOSED 2026-08-22, not reproduced**

**Outcome (step 5 of the procedure below).** The DDR-968 instrument has been live
since `ea4601e` and has never printed. That is not a gap in the investigation —
it is the measurement: the line is emitted only while the witness is unarmed,
so a green boot emits zero of them by design (DDR-968 §3). `smoke-agents` is a
**gating** test (`tools/ci/gate_shards.txt`: shard 2; absent from the CI
`EXCLUDE` list), so any recurrence would have failed its entire check suite —
and 18 suites have been green on shard 2 since. There is no artefact to read and
no named mechanism, so §NON-NEGOTIABLE 3 forbids a fix here. Instrument stays
armed; the issue reopens on the first `PRADYOS_AGENT_WITNESS_WAIT` line.

*Original procedure, retained:*

1. Read the DDR-968 artefact now — the instrumentation is already live.
2. Root-cause the rqdepth=11 stall from the existing witness data.
3. Fix the scheduler/AETHER interaction causing the freeze.
4. Gate: `smoke-agents` 3× CI green.
5. If the artefact is ambiguous after 2 independent approaches: add one more
   instrument, push, harvest next CI run, then fix.

### ITEM 3 — Merge PR #5 into `dev/phase1`

- 3 consecutive CI greens on the SAME tip SHA (`gh run rerun` for the third)
- `ci-shard-check` green, `ci-probe-rodata-check` green
- Remove draft status, then **squash-merge** into `dev/phase1`
- **HOLD IS LIFTED** — merge immediately when greens are confirmed.

---

## PHASE 2 — FULL BACKLOG (after PR #5 merges)

---

### GROUP A — Kernel Completeness

| Item | Detail | Gate |
|---|---|---|
| ~~Demand-paged user stack~~ | **ALREADY BUILT — ADR-038.** `vmm_stack_fault()` (`vmm_cow.c:144`) faults the stack in a page at a time; `USER_STACK_EAGER_PAGES = 8` is measured, not guessed (the ADR's own A/B: 30/30 eager vs 0/30 at one page, because `vmm_user_range_ok` validates syscall pointers WITHOUT faulting); guard page below `USER_STACK_BOT`. Gate is **`smoke-stack-demand`** (3 arms: grow, syscall-on-grown-page, guard-kill with a post-kill liveness witness) — `smoke-lazystack` does not exist. Do NOT rebuild. | `smoke-stack-demand` ✅ |
| I/O APIC migration | DDR-714 stage D — disable 8259, route ISA IRQs through I/O APIC | `smoke-ioapic` |
| SMEP / SMAP | **BOTH DONE — SMEP DDR-1040, SMAP DDR-1041.** CR4 bit 20, CPUID-guarded, set on the BSP and on every AP (CR4 is per-CPU). Safe by construction here and the DDR proves it from the page tables: stage2.asm builds the identity map `0x83` and the higher half `0x3`, U=0 at every level, and `VMM_USER_MIN >> 39 == 1`, so every user mapping lives in PML4 slot 1 — no address the kernel executes from has U=1. **THE VACUITY TRAP, MEASURED FIRST:** the TCG default `qemu64` reports `smep=false` / `smap=false` (QMP `query-cpu-model-expansion`), so a correct CPUID-guarded implementation is a permanent no-op in CI and any 'SMEP enabled' assertion would be unreachable-passing forever — `smoke-smep` therefore **pins its own `-cpu qemu64,+smep`**, and asserts the no-op path in a second boot on the default model. Ships the **one-shot expected-fault latch** (`kernel/fault_expect.h`), without which enforcement is unobservable: a ring-0 #PF is fatal here, so 'the CR4 bit is set' would have been the only assertable claim, i.e. decoration. The probe `jmp`s (not `call`s) to a user page because the faulting RIP is the TARGET, and a `call` would leave RSP 8 low on resume. M1/M2/M3 measured on three distinct hashes; **M2 fails arm B alone**, which is what proves the arm measures user-ness rather than 'some fault happened'; **M3 passes every arm** — the latch's RIP-window check is measured-uncovered, not assumed-covered. **`smoke-wx`, the gate name this row used to carry, does not exist** — the real kernel-W^X gate is `smoke-wxkernel`. **SMAP (DDR-1041)** turns `uaccess.h`'s header claim — 'the kernel NEVER dereferences a raw user pointer anywhere else' — from a documented contract into one the hardware checks. `stac`/`clac` are a RUNTIME BRANCH on `g_smap_on`, not unconditional, because both instructions are `#UD` without SMAP. The window opens AFTER `vmm_user_range_ok` (so a bad pointer is still rejected with AC clear) and, in `copyinstr`, around the SINGLE BYTE rather than hoisted out of the loop (hoisting would hold AC across the page-boundary revalidation, i.e. across kernel page-table reads). **THE ENUMERATION WAS MEASURED, NOT GREPPED:** a full boot with SMAP on is line-for-line equivalent to baseline (416 vs 418, same steady state), and 19 user-pointer-dense gates re-run with `+smap` are all rc=0 — **the contract HOLDS, no `stac` was needed anywhere outside `uaccess.c`**. Three of those 19 first looked vacuous (they set their own `SERIAL_LOG`); every run is verified to have had the feature ON, which is the check DDR-1023 recorded the absence of. M2 (shield emptied) PANICS `smoke-fs` with `#PF error=0x01`, measured not predicted. **NOT FIXED and named: an interrupt taken between `uaccess_begin` and `uaccess_end` runs with AC still set** — the CPU clears IF on an interrupt gate, not AC — so SMAP is off inside that window; fixing it means touching `isr_common`, which is load-bearing for DDR-981/1006/1010 and the open OPEN-2. | `smoke-smep` ✅ (shard 5) + `smoke-smap` ✅ (shard 0) |
| Kernel W^X identity-alias removal | `vmm_protect_kernel()` — remove identity alias (DDR-757 residual). **Gate name corrected: `smoke-wxkernel`, not `smoke-wx` (which does not exist) — DDR-1040 §7.2.** | `smoke-wxkernel` |
| `#MC` machine-check handler | Panic with full register state | `smoke-mc` |
| KASLR | After W^X is CI-green | `smoke-kaslr` |
| ~~Scheduler timed-block~~ | **ALREADY BUILT — DDR-955.** The name in this row is a placeholder that never existed; the shipped call is **`sched_block_timeout(spinlock_t *lk, volatile int *done, uint64_t timeout_ticks)`** (`sched.c:1434`), same locking contract as `sched_block_on` (called with `lk` held, returns with it held), returning `-ETIMEDOUT`. The expiry sweep is in `sched_tick` (`sched.c:1287`) and `struct tcb` carries `block_deadline` + `wake_timed_out`. Four callers: `virtio_blk.c:232` and `:288`, `bcast.c:78`, `ipc.c:65`. **Do NOT rebuild.** The genuinely unbounded wait is elsewhere and is now tracked as DDR-994: `mnt_lock` (`vfs/vfs.c:25`) is a bare `while (exchange(&m->busy,1)) yield();` with no deadline at all — DDR-981 fixed the interrupt masking inside `yield()` but never bounded the spin, which is exactly OPEN-1 route 1's signature (cpu busy, thread never progresses, nothing printed). | `smoke-schedtimeout` (unwritten; the four call sites are gated by their own gates) |
| Per-CPU `sched_exit` / zombie reap under full SMP | — | existing SMP gates |
| `smoke-rqstress` determinism | 20× green before moving on | `smoke-rqstress` 20× |
| Spinlock contention instrumentation | `lock_stat` hold-time + contention counts | `smoke-lockstat` |
| ~~B#3 AP-liveness fix~~ | **FIXED — DDR-981.** Neither a block-layer nor a LAPIC bug: `yield()` spun with `RFLAGS.IF` clear because SYSCALL entry masks it and never restores it. Note the gate lesson that motivated the new sentinel — `smoke-smp` and `smoke-rqstress` both measured **20/20** at `-smp 4` while the defect was live, i.e. the GATES DID NOT CATCH IT; the only evidence was `[vblk] compl wait timeout` sitting in a serial log nobody asserted on. That is now fixed at the source: `[apfreeze]` is in `GLOBAL_FORBIDDEN`. | `[apfreeze]` in `GLOBAL_FORBIDDEN` ✅ + 20/20 at `-smp 4` ✅ |
| **smoke-smpuser fix** | Measure g_ticks at main.c:1134 and main.c:1311. Branch (B) = large gap → scheduler starvation fix. | `smoke-smpuser` |

---

### GROUP B — Storage / Filesystem

| Item | Detail | Gate |
|---|---|---|
| Provisioned SFS as default boot root | Gate `sfs_format` at `main.c:1128` behind `probe_enabled()`. Update the 12 gates asserting on `[sfs]` self-test sentinels. | `smoke-sfs-boot-root` |
| ~~FAT32 multi-cluster read fix~~ | **DONE — DDR-973, as a refutation + gate.** No defect found: the symptom does not reproduce and the named function does not exist. Probe shipped as `user/fat32mctest.c`. NOTE the pattern: plain `7n+3` has period 256, so with 512-byte clusters every cluster is identical and the gate is VACUOUS — a chain-repeat mutant passed it. The shipped pattern is `(7n + 3 + 31*(n>>8)) & 0xFF`. | `smoke-fat32-multicluster` ✅ |
| SFS on-disk free-tree persistence | — | `smoke-sfs-persist` |
| SFS B+tree CoW GC | — | `smoke-sfs-gc` |
| SFS extent overflow / large files | — | `smoke-sfs-largefile` |
| `mkfs.sfs` >512 slots / deeper trees | — | `smoke-sfs-deepslot` |
| SFS free-space quotas / per-mount limits | — | `smoke-sfs-quota` |
| B#6 ext4 write | — | `smoke-ext4-write` |
| B#9 I/O APIC (storage path) | Depends on Group A I/O APIC item | `smoke-ioapic` |
| B#10 NUMA affinity | — | `smoke-numa` |
| B#14 NAS 3-lane storage scheduler | — | `smoke-nas` |
| B#15 PMM policy | — | `smoke-pmmpolicy` |
| B#1 NVMe IRQ | On hold until B#3 SMP is fully stable — resume when safe | `smoke-nvmeirq` |
| ~~OPEN-10 B+tree SMP fix~~ | **FIXED — DDR-964** (create-then-init race; see §OPEN ISSUES). Pending CI promotion evidence only. Do NOT re-root-cause. | `smoke-sfs-btree-smp4` |

---

### GROUP C — Networking (NET-C and beyond)

| Item | Gate |
|---|---|
| `epoll` / `select` for proxy sockets | `smoke-epoll` |
| UDP send / raw socket API | `smoke-udp` |
| `SYS_NET_REVOKE` / CAP_NET policy reload | `smoke-netrevoke` |
| True peer loopback / TAP netdev | `smoke-tap` |
| IPv6 (after NET-C stable) | `smoke-ipv6` |
| TLS shim — mbedTLS or equivalent; no out-of-tree libs in OS image | `smoke-tls` |

---

### GROUP D — Userspace / PRISM Shell

| Item | Detail | Gate |
|---|---|---|
| `argv`/`envp` marshalling in `sys_execve` | — | `smoke-execve-argv` |
| PRISM RX line discipline / echo / readline | **ERASE DONE — DDR-1039.** `readline()` appended every non-newline byte, so a backspace landed *in* the command: `hepl`+2 erases parsed as `hepl\x7f\x7flp` and matched no builtin. Invisible to all 170 gates because every one of them injects byte-perfect lines. 0x7F and 0x08 both honoured. **Echo deliberately NOT added** (DDR-1039 §2): PRISM shares COM1 with the kernel, so echoing typed input would put it into the serial log that every gate asserts on, and without termios the host terminal's own echo would double it. Column-zero guard recorded UNCOVERED — from outside the shell, erasing nothing and erasing at column zero are identical. Remaining: echo, arrows/cursor, `^W`, `^U`, history. **The `smoke-readline` name in this row does not exist and should not be built** — the arm belongs on `smoke-shell`, where PRISM's line handling already runs; a separate gate would boot an OS to type one word. | `smoke-shell` (erase arm) ✅ |
| ~~PRISM `run` re-enable~~ | **NOT DISABLED — nothing to re-enable (DDR-973 §7).** `user/prism.c` dispatches `run`; `do_run`/`do_run_bg` fork+execve; `smoke-shell` already runs `run /EXECTEST.ELF` twice plus `jobs`/`fg`. What ADR-024 §D5 deferred is narrower: **init-driven fork+execve RESPAWN of PRISM**, which is unbuilt work, not a blocked item. Row kept for that. | `smoke-shell` (existing) |
| PRISM pipes / redirection / quoting / job control / scripting | — | `smoke-pipes` |
| `SYS_MPROTECT` | — | `smoke-mprotect` |
| `SYS_POLL` | — | `smoke-poll` |
| `SYS_FUTEX` | — | `smoke-futex` |
| `pthread` / ring-3 threading | `clone(CLONE_VM\|CLONE_FILES\|CLONE_THREAD)` | `smoke-pthreads` |
| 6-arg `sys_mmap` ABI widening | — | `smoke-mmap6` |
| `mmap` file-backed mappings | page-fault handler, dirty tracking, `msync` | `smoke-mmap-file` |
| Dynamic linking | `ld.so` / musl dynamic linker, `.so` in ELF loader | `smoke-dynlink` |
| `io_uring` completions | `OP_FSYNC`, `OP_OPENAT`, eventfd, SQE chaining | `smoke-iouring` |
| `SYS_SIGACTION` full POSIX | `SA_RESTART`, `SA_SIGINFO`, `sigprocmask`, `sigaltstack`, `SIGCHLD` | `smoke-sigaction` |
| PRISM `ls -R` / `ps` full | open-fd listing, recursive ls, signal-mask display | `smoke-prism-ls` |
| B#12 PRISM job control | `$?`/SIGPIPE ✅ — remaining: full job control, `&`, `wait`, `fg`/`bg` | `smoke-jobctl` |
| B#13 dynamic linker | Same as dynamic linking above | — |

---

### GROUP E — Compositor / Desktop (Layer 7 remaining)

| Item | Detail | Gate |
|---|---|---|
| PS/2 modifier keys | F-keys, arrows, Alt, Ctrl, Meta/Super | `smoke-modkeys` |
| Super+M physical binding | `SYS_SET_MODE` sovereign toggle via physical key | `smoke-superkey` |
| Alt-Tab with modifier plumbing | Upgrade from plain Tab (DDR-720) | `smoke-alttab` |
| ~~Ctrl+Alt+T~~ | **DONE — DDR-1027.** Not a keybinding: PRISM reads fd 0 / writes fd 1, so there was no terminal *window* to launch. Ships `user/term.c` — a client that owns a surface, runs PRISM over a pipe pair, and renders with the Inter atlas. It is an **epoll** client because this kernel has no `O_NONBLOCK`: a plain `read()` on PRISM's stdout would block and the window would stop draining its key ring. `fork`+`execve`, not `SYS_SPAWN_AGENT` (a terminal is an application, not a roster agent). Five arms; **arm E** prints `PRADYOS_TERM_CHORD mods= spawn=` for every `t` press, because `input_inject.sh` replays its list 4× and the cap is 4, so spawn *counts* cannot discriminate the chord. `term.elf` is not embedded in the kernel, so record ITS hash — the term-side mutants leave `kernel.bin` bit-identical. | `smoke-ctrlaltt` ✅ |
| Per-window restore from dock | DDR-717 restores all; add per-tile | `smoke-perrestore` |
| Window maximize at real display size | DDR-719 caps at 512×512; lift to real geometry | `smoke-maximize` |
| ~~Pointer resize handles — all edges~~ | **DONE — DDR-997.** Eight regions, 14 px; SE unchanged bit-for-bit. A W/N drag needs a MOVE *and* a resize through two non-atomic syscalls — move first, and clamp the size BEFORE deriving the origin (clamping after leaves the fixed edge sliding). Mutation-checked M1/M2/M3, three distinct kernel hashes; M3 fails `smoke-drag` because BETA's published `dg=` sits inside ALPHA's east band. Also fixed here: `PRADYOS_WM_GEOM` was republished only on a surface-count or focus change, so it was stale after any move or resize. | `smoke-resizeall` ✅ |
| ~~`SURF_EV_CLOSE` notification~~ | **DONE — DDR-998.** Event type 4 (1/2/3 were already resize/scroll/composited). The compositor ASKS, then forces after a bounded grace; the owner may delay, never veto. A surface id does not identify a surface — 16 slots recycle immediately — so `struct surface` gained a generation counter, and `surf_take_free`'s whole-struct wipe had to be taught to preserve it. M1b/M2 mutation-checked on distinct hashes and they fail DIFFERENT arms; M3 (recycle guard) recorded UNMEASURED with its reason. | `smoke-surfclose` ✅ |
| ~~Compositor double-map `PTE_SW_SHARED` audit~~ | **DONE — DDR-1003. Invariant HOLDS**, all 9 `vmm_map_in` sites enumerated: the four genuinely double-mapped frames (surface views, framebuffer, vDSO, metric page) all carry the bit, and `fork` aliases shared frames verbatim while refcounting the rest, so no double-free. Found instead: **`ptnode_in_use` underflows on every fork** — it counts `++` per FRAME but `--` per MAPPING, and `vmm_cow_fault` drops its old ref with `pmm_free_page` (no `--`). RO text pages are shared with no COW at all, so they can never rebalance. NOT fixed: no gate reads it across a fork, so there is no artefact (§NON-NEGOTIABLE 3). DDR-1003 §5.1 says what the gate must do — and warns the obvious leak-gate shape is balanced and would pass. | `smoke-sharedpte` (unbuilt; DDR-1003 §5.1) |
| OKLab horizon bands / animated mesh | DDR-716 deferred mesh + horizon bands | `smoke-horizon` |
| ~~vDSO callable reader (`vdso_entry.asm`)~~ | **ROW CORRECTED — DDR-1005.** The ring-3 reader EXISTS and is GATED: `user/systest.asm:41/346` loads `VDSO_VA 0x7FFFFFF00000` and prints `VDSO: clock ns=`, which `smoke-vdso` asserts (shard 7, **strict**) — a user-mode sentinel, so the gate is not kernel-side and not vacuous. `actiondagtest.c:97` and `compositor.c:750` read it in anger. The **seqlock is unnecessary, not deferred**: one aligned `uint64_t` is atomically loaded on x86_64, so nothing can tear — it becomes owed when a SECOND field lands, and `vdso_page.h` already says so. `vdso_entry.asm` (a *callable* entry) is the only genuinely unbuilt part and is a **security-posture change**, not a checkbox: the user view is deliberately `VMM_NX`, and making it executable would leave a frame that ring 0 can rewrite and ring 3 can execute — for the gain of a `call` over a `mov`. Do NOT create `smoke-vdso-read`; it would duplicate `smoke-vdso`. | `smoke-vdso` ✅ (shard 7) |
| **OPEN-1 fix** | `smoke-surfdestroy` intermittent. Add instrumentation. Get artefact. Root-cause. Fix. Active. | `smoke-surfdestroy` 20× |

---

### GROUP F — AETHER / Agent Layer

| Item | Detail | Gate |
|---|---|---|
| AETHER audit ring → SFS persistence | `/etc/aether/audit.log` — needs SFS boot root first (Group B item 1) | `smoke-auditpersist` |
| Agent `execve`-on-respawn from SFS | `/agents/kryos.elf` etc. — needs FAT32 fix or SFS as agent root | `smoke-agentexec` |
| Multi-agent concurrency arbitration | Per-agent quota + priority queue | `smoke-agentconc` |
| Per-agent live-metrics panel | CPU% sparkline, memory graph, action-rate histogram | `smoke-agentmetrics` |
| `SYS_AGENT_ROSTER` / `SYS_AGENT_METRICS` liveness continuity | — | `smoke-rosterctd` |
| Section 3C `ACTION_READ_FILE` | — | gate per type |
| Section 3C `ACTION_DELETE_FILE` | — | gate per type |
| Section 3C `ACTION_SEND_IPC` | — | gate per type |
| Section 3C `ACTION_QUERY_MEMORY` | — | gate per type |
| Section 3C `ACTION_REWRITE_AGENT_CODE` | — | gate per type |
| Section 3C `ACTION_PROPOSE_HYPOTHESIS` | — | gate per type |
| Section 3C `ACTION_RUN_EXPERIMENT` | — | gate per type |
| Section 3C `ACTION_EVOLVE_GENOME` | — | gate per type |
| Section 3D daemon features #45–#65 | **21 of 21 COMPLETE** ✅ — do NOT rebuild. Verified DDR-846–856. | — |
| F#66 `architect_agent` | ⬜ not started | `smoke-architect` |
| F#67 `healer_agent` | ⬜ not started | `smoke-healer` |
| F#68 metric lockbox | **SHIPPED + GATED** — `smoke-lockbox` (shard 7, strict), DDR-812 | `smoke-lockbox` ✅ |
| F#69 `inventor_agent` | ⬜ not started | `smoke-inventor` |
| F#70 `tournament_agent` | ⬜ not started | `smoke-tournament` |
| F#71 subconscious world model | ⬜ not started | `smoke-worldmodel` |
| F#72 `verifier_agent` | ⬜ not started | `smoke-verifier` |
| F#73 sovereign NL UI | ⬜ not started | `smoke-nlui` |
| F#74 capability discovery | ⬜ not started | `smoke-capdiscovery` |
| F#75 lineage memory | ⬜ not started | `smoke-lineage` |
| F#76 tamper-evident ledger | **SHIPPED + GATED ×2** — `smoke-auditchain` (shard 0) + `smoke-auditchain-tamper` (shard 4), both strict | ✅ |
| Section G: 4 remaining roster slots | subconscious, ai_scientist, architect, tournament | `smoke-g-slots` |
| `CAP_OCR` (1<<19) wiring + enforcement gate | — | `smoke-capocr` |
| `CAP_EXEC` (1<<20) wiring | Wire so `shell_agent` (PRAX) is spawnable | `smoke-capexec` |
| `CAP_SCENE` (1<<22) wiring | Wire so `vision_agent` (IRIS) is spawnable | `smoke-capscene` |
| `CAP_NET_BROWSE` (1<<23) wiring | Wire so `research_agent` (LUMYN) is spawnable | `smoke-capnetbrowse` |
| Make PRAX (shell_agent) spawnable | After CAP_EXEC wired | `smoke-prax` |
| Make LUMYN (research_agent) spawnable | After CAP_NET_BROWSE wired | `smoke-lumyn` |
| Make AHNIS (ocr_agent) spawnable | After CAP_OCR wired | `smoke-ahnis` |
| Make IRIS (vision_agent) spawnable | After CAP_SCENE wired | `smoke-iris` |
| RUFLO (healer_agent) spawnable | — | `smoke-ruflo` |
| S3 + S7 invariant arms | Depend on F#66–F#72 | extend `smoke-invariants` |
| ~~OPEN-2 SMP intermittent fix~~ | **CLOSED — DDR-981** (downstream of B#3; see §OPEN ISSUES). DDR-863 was the wrong lead. Do NOT re-root-cause without an `[apfreeze]` artefact. | `[apfreeze]` in `GLOBAL_FORBIDDEN` ✅ |

---

### GROUP G — Phase 9 Assembly Optimization (6 of 7 items ⬜)

For each item: **profile first** (add timing instrumentation), establish baseline,
implement, measure improvement. Gate must show measurable speedup in a
deterministic test.

| Item | Detail |
|---|---|
| Phase 9.1 | hot-path `kputc` optimization — profile and optimize |
| Phase 9.2 | context-switch critical path — `sched.c` save/restore cycle count reduction |
| Phase 9.3 | TLB shootdown batching under SMP |
| Phase 9.4 | virtio-blk submission batch path — reduce doorbell writes |
| Phase 9.5 | IPC fast path — single-copy where possible |
| Phase 9.6 | page-table walker SIMD — SSE2 for bulk zero-page mapping |

---

### GROUP H — Release (DEADLINE: 2026-08-24 23:59 UTC)

| Item | Detail | Gate / Action |
|---|---|---|
| ISO x86_64 | multiboot2 + grub-mkrescue | `smoke-iso-x86_64` |
| ISO aarch64 | EFI/U-Boot packaging (kernel already boots in CI — packaging only) | `smoke-iso-aarch64` |
| ISO riscv64 | OpenSBI + U-Boot packaging (kernel already boots in CI — packaging only) | `smoke-iso-riscv64` |
| `prad` package manager | NSI **88–90** (87 is `SYS_READ_AUDIT` — do NOT reuse). Per BUILD_TRACKER TASK 18. | `smoke-prad` |
| Full invariant gate suite S1–S8 | S3/S7 depend on Group F (F#66–F#72). S1,S2,S4,S5,S6,S8 already pass. | extend `smoke-invariants` |
| 3× consecutive CI greens on `main` tip | Before tagging | `gh run rerun` |
| Tag `v1.0.0` on `main` | **This is the finish line** | `git tag v1.0.0` |

---

## PHASE 3 — Quantum Layer (Phase 10) — BUILD IMMEDIATELY AFTER v1.0.0

**The quantum layer is NOT deferred indefinitely. Build it right after the ISO
ships and `v1.0.0` is tagged. Do NOT pull it forward before v1.0.0.**

| Item | Detail | Gate |
|---|---|---|
| QAL (Quantum Abstraction Layer) | Kernel API: `SYS_QPU_SUBMIT`, `SYS_QPU_READ`, `SYS_QPU_STATUS`. Gate behind `CAP_QUANTUM`. | `smoke-qpu` |
| Virtual QPU emulator | Software 5-qubit QPU for CI. State vector simulation, H/CNOT/T/S/Rz gates. No real hardware required. | `smoke-qpu-sim` |
| QAOA scheduler | Quantum Approximate Optimization Algorithm for process scheduling hints. Runs on virtual QPU. | `smoke-qaoa` |
| Hybrid classical-quantum API | ring-3 hybrid programs: submit circuit, block for result, continue classically. `user/qaoatest.c`. | `smoke-hybrid-api` |

---

## PRE-APPROVED EXCEPTIONS — log these, do NOT build before v1.0.0

For each: add a one-line entry in `docs/BUILD_TRACKER.md` as `[DEFERRED: reason]`.

| Item | Log as |
|---|---|
| Intel HDA audio | "deferred, optional — no QEMU HDA path in CI" |
| Wayland/wlroots compositor | "superseded by shipped custom C framebuffer compositor" |
| CMake/Makefile hybrid | "deferred post-1.0, awaiting operator sign-off (DDR-843)" |
| Apple Silicon / m1n1 | "deferred post-1.0 — aarch64 ISO uses U-Boot path" |
| `ACTION_CAPTURE_FRAME` | "post-L7, no hardware path" |
| `ACTION_SCAN_ENVIRONMENT` | "post-L7, needs SLAM3R" |
| `ACTION_QUERY_SCENE` | "post-L7, no scene graph" |
| `ACTION_PARSE_DOCUMENT` | "needs 64 MiB OCR model, no model-shipping path" |
| `ACTION_EXEC_CODE` | "needs sandboxed interpreter subsystem" |
| `ACTION_BROWSE_WEB` | "deferred post-1.0 (DDR-793) — cloud bridge is a security-posture change" |
| `arch/aarch64` full port | "boot-only scope per ADR-034 — ISO uses boot-only kernel" |
| `arch/riscv64` full port | "boot-only scope per ADR-034 — ISO uses boot-only kernel" |
| Cloud bridge activation | "deferred post-1.0 (DDR-793)" |
| Rust rewrite | "not in scope" |
| `CAP_OCR`, `CAP_SCENE` if no hardware path | "capability bit defined, enforcement deferred — no subsystem path" |
| SFS block reclamation on-disk | "in-memory reclaim shipped (DDR-762-v2); on-disk free-tree deferred post-1.0" |
| NVMe completion IRQ | "poll-mode sufficient for ISO; DDR-774a/b/c deferred until B#3 SMP stable" |

---

## WHAT "DONE" MEANS — DEADLINE 2026-08-28 23:59 UTC

Every box must be checked before the deadline:

- [ ] `make image` exits 0, zero warnings at `-Werror`
- [ ] All items CI-green or carrying a logged pre-approved exception
- [ ] `ci-shard-check` green
- [ ] `ci-probe-rodata-check` green
- [ ] `kernel.bin` ≤ 1,572,864 B
- [ ] `docs/AETHER_MASTER_FEATURES.md` fully up to date
- [ ] `docs/BUILD_TRACKER.md` fully up to date
- [ ] `SESSION_HANDOFF.md` updated on every commit
- [ ] PR #5 squash-merged into `dev/phase1` (3 CI greens) ← HOLD LIFTED
- [ ] `dev/phase1` fast-forwarded to `main` (3 CI greens on same tip)
- [ ] All Groups A–H CI-green or pre-approved-excepted
- [ ] All open issues CLOSED. Done: OPEN-2 + B#3 (DDR-981), OPEN-10 (DDR-964),
      FSRM (DDR-967), smoke-agents (not-reproduced, DDR-968), FAT32
      multi-cluster (refuted + gated, DDR-973), Dependabot (already remediated).
      Remaining: OPEN-1, OPEN-12, OPEN-13.
- [ ] x86_64 ISO built and bootable
- [ ] aarch64 ISO built and bootable
- [ ] riscv64 ISO built and bootable
- [ ] `v1.0.0` tagged on `main`
- [ ] Zero open issues, zero warnings, zero unlogged exclusions

**ISO must be testable, and `v1.0.0` tagged on `main`, by 2026-08-28 23:59 UTC**
(extended from 2026-08-24 by `docs/OPERATOR_DIRECTIVE_2026-08-23.md` §1 — that
directive supersedes the older date wherever this file still implies it).
**After v1.0.0 is tagged: begin Phase 10 (Quantum Layer) immediately.**

**Begin with Phase 1 Item 1 (FSRM fix). Parallelize across groups. Do not stop.**

---

## HYGIENE GATES — must pass before EVERY commit

1. `build/kernel.bin` warning-clean (`-Werror` clang + nasm).
2. **`bash tools/ci/hygiene_check.sh`** — runs ALL FOUR static checks
   (DDR-1042 added `ci-resizecheck-selftest`; the wording below still says three
   because it describes the historical three — the SCRIPT is authoritative, which
   is the whole point of this item)
   (`ci-shard-check`, `ci-probe-rodata-check`, **`ci-start-align-check`**) and
   reports each rc. **Run the script, not a list of target names.** This item
   read "`make ci-probe-rodata-check`" and item 5 read "`make ci-shard-check`",
   naming two of the three; `ci-start-align-check` was missing from this list
   while CI ran it (`.github/workflows/ci.yml:35`). DDR-1015 shipped a probe
   whose `_start` lacked `force_align_arg_pointer` straight through this gap —
   found and fixed in DDR-1016 §7. A list of names drifts; the script cannot.
3. No `[BUG]` lines in serial log.
4. `make smoke-blkmq` exits rc=0.
5. (folded into 2 — `ci-shard-check` runs there.)
6. `make smoke-rqstress-liveness` exits rc=0.
7. `make smoke-blk-integrity` exits rc=0.
8. `make smoke-shell` 5/5 locally.

---

## ORIENTATION

- **Pre-launch checklist (every deferred/open item, one document): `docs/PRE_LAUNCH_CHECKLIST.md`**
- Status: `docs/build_status.md`
- Feature state: `docs/AETHER_MASTER_FEATURES.md` (Sections A–H)
- Full backlog: `docs/BUILD_TRACKER.md`
- Decisions: `docs/decisions/ADR-*.md`
- Session state: `SESSION_HANDOFF.md` (repo root — NOT docs/)
- DDR numbering: `docs/ddr/DDR-NUMBERING-MAP.md` (free range: DDR-936+)
- Graph: `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`

---

## DEFERRED — DO NOT PULL FORWARD BEFORE v1.0.0

- Phase 10 quantum layer — build AFTER v1.0.0 is tagged
- `arch/aarch64` / `arch/riscv64` full ports (boot-only scope, ISOs use that)
- Apple Silicon / m1n1
- Rust rewrite of any component
- Cloud bridge activation (DDR-793)
- CMake/Makefile hybrid (DDR-843)
- `ACTION_BROWSE_WEB` (DDR-793)
- SFS on-disk free-tree persistence (DDR-762-v2 shipped in-memory reclaim)
- NVMe completion IRQ (DDR-774a/b/c — after B#3 SMP stable)
