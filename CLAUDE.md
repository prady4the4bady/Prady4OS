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
   - B#3 virtio-blk SMP stall must be fixed before ISO.
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
6. **`GLOBAL_FORBIDDEN` is append-only.** Never remove a sentinel.
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
Free range: **DDR-936+**. Before allocating ANY DDR number:
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
Last shipped: **NSI 74** (`SYS_MEMINFO`). Next free: **75**. Table size: **128**.
(Full NSI map: 0–46 Layer-2..6 syscalls; 47=`SYS_MOUSE_POLL`; 48–63=surface;
64=`SYS_AGENT_ROSTER`; 65=`SYS_NET_ALLOW`; 66=`SYS_GETDENTS`;
67=`SYS_GETPROCS`; 68=`SYS_UNLINK`; 69=`SYS_POWEROFF`; 70=`SYS_REBOOT`;
71=`SYS_SYSINFO`; 72=`SYS_TIME`; 73=`SYS_DMESG`; 74=`SYS_MEMINFO`.)

### §INV.15 — Three CI greens rule
A push yields exactly 2 suites per commit (push + pull_request events). A third
green requires `gh run rerun` on the same SHA. "Both suites green" does NOT
satisfy the 3-green rule.

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
the Makefile size gate at 1,572,864 B.
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
| **smoke-agents preempt frozen** | `rqdepth=11`, two sentinels missing | One CI capture shard 2 | **ITEM 2 — ACTIVE FIX REQUIRED.** Read the DDR-968 artefact. Root-cause and fix. Do not wait for another CI red. |
| **OPEN-1** | `smoke-surfdestroy` intermittently misses sentinel | Unknown | **ACTIVE FIX.** Add instrumentation to `surfdestroy` path. Get a failing artefact. Root-cause and fix. |
| **OPEN-2** | `smoke-resched`, `smoke-blkmq-trace`, `smoke-msixap`, `smoke-crosswake` intermittent | All `QEMU_SMP=4` gates — DDR-863 | **ACTIVE FIX.** These are SMP timing issues. Reproduce at `-smp 4` locally. Apply targeted fix from DDR-863. |
| **OPEN-10** | `btree churn FAIL` during unrelated SMP gates | **ROOT-CAUSED — the create-then-init race, DDR-964.** `rc=-1` is `-EPERM` (`EPERM==1`) from `cap_ok(cap, CAP_FS_WRITE)`: `sched_create()` made a thread runnable before its caller minted the capability into `->arg`, so a thread picked early ran with `CAP_NULL`. NOT a separate defect from the row at §CURRENT BUILD STATE — this symptom **is** OPEN-10 and DDR-964 is its fix; the two rows contradicted each other and this one was the stale half. | **FIXED (DDR-964), pending CI promotion evidence.** `smoke-sfs-btree-smp4` stays excluded until greens accumulate. |
| **OPEN-11** | `smoke-sha256`, `smoke-rqstress-liveness` | Scratch LBA 1500 overwrote kernel image | **CLOSED — DDR-831.** Do not revisit. |
| ~~Uninit PID~~ | ~~`AGENT_OOM_KILLED` prints garbage PID~~ | **NOT garbage — it is `AE_TEST_PID` (`0xA37E0000`), the self-test's deliberate sentinel, `#define`d at `aether.c:14`** | **CLOSED as a non-bug, DDR-969.** Do not reopen. |
| **FAT32 large-file** | `execve` of large musl ELF corrupts | multi-cluster `read_cluster_chain` bug (ADR-024) | **Fix in Group B.** Do not wait. |
| **Dependabot** | 5 alerts (2 high, 3 moderate) | Third-party deps | **Triage and fix the 2 high-severity ones.** Moderate: fix if quick, else log. |
| **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall | timer/IRQ delivery under SMP | **MUST be fixed before ISO.** This is BLOCKING the release. Active work required. |
| **smoke-smpuser B#3** | `[smp] user on AP OK` never appears | Scheduler starvation (DDR-777 branch B) | **Measure g_ticks stamps at main.c:1134 and main.c:1311.** Gap predicts which branch. Fix from artefact. |

---

## CURRENT BUILD STATE

- **Gate count: 147** assigned across 6 shards, 6 excluded (`ci-shard-check`,
  verified 2026-08-22). The "105" this line used to carry was long stale.
- **NSI max: 93** (`SYS_VERIFY_AUDIT`). **Next free: 94.** Table size: 128.
  (This line used to say 74 / `SYS_MEMINFO`, contradicting §INV.14 in the same
  file. §INV.14 was right.)
- **`kernel.bin`**: **1,053,054 B** against the 1,572,864 B size gate — 519,810 B
  of headroom. The old "~545 KiB, 768 KiB ceiling" was stale in both terms.
- **DDR free range: DDR-936+**
- `make image` → zero warnings, `-Werror` enforced ✅
- PR #5 tip: `ea4601e` — draft, base `dev/phase1`
- Three intermittents fixed: OPEN-10 (DDR-964), smoke-cadence (DDR-965), Item 48 (DDR-966)
- FSRM: **FIXED — DDR-967**, `smoke-fsrm` 20/20 local
- smoke-agents: **instrumented — DDR-968**, no fix yet; awaiting CI artefact
- Overall completion: ~79% (~66+ items remain across all groups)

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

### ITEM 2 — Resolve smoke-agents (preempt frozen / rqdepth=11)

**Active fix required — do not wait for another CI red:**
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
| SMEP / SMAP | `CLAC`/`STAC` around every copyin/copyout | `smoke-smep` |
| Kernel W^X identity-alias removal | `vmm_protect_kernel()` — remove identity alias (DDR-757 residual) | `smoke-wx` |
| `#MC` machine-check handler | Panic with full register state | `smoke-mc` |
| KASLR | After W^X is CI-green | `smoke-kaslr` |
| Scheduler timed-block | `sched_block_on_timeout(&lk, deadline)` — implement AFTER g_ticks is CI-proven reliable | `smoke-schedtimeout` |
| Per-CPU `sched_exit` / zombie reap under full SMP | — | existing SMP gates |
| `smoke-rqstress` determinism | 20× green before moving on | `smoke-rqstress` 20× |
| Spinlock contention instrumentation | `lock_stat` hold-time + contention counts | `smoke-lockstat` |
| **B#3 virtio-blk SMP stall fix** | **BLOCKING ISO.** Root-cause the `-smp 4` completion stall per DDR-806. Measure g_ticks stamps first. | `smoke-smp` 20× |
| **smoke-smpuser fix** | Measure g_ticks at main.c:1134 and main.c:1311. Branch (B) = large gap → scheduler starvation fix. | `smoke-smpuser` |

---

### GROUP B — Storage / Filesystem

| Item | Detail | Gate |
|---|---|---|
| Provisioned SFS as default boot root | Gate `sfs_format` at `main.c:1128` behind `probe_enabled()`. Update the 12 gates asserting on `[sfs]` self-test sentinels. | `smoke-sfs-boot-root` |
| **FAT32 multi-cluster read fix** | `execve` of large musl-C ELF corrupts — root-cause via `read_cluster_chain` for >1-cluster ELFs. Probe: `fat32_multicluster.c`, pattern 7n+3 across cluster boundary. (Open since ADR-024) **ACTIVE FIX.** | `smoke-fat32-multicluster` |
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
| PRISM RX line discipline / echo / readline | — | `smoke-readline` |
| PRISM `run` re-enable | After FAT32 multi-cluster fix | `smoke-prism-run` |
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
| Ctrl+Alt+T | Launch PRISM terminal window | `smoke-ctrlaltt` |
| Per-window restore from dock | DDR-717 restores all; add per-tile | `smoke-perrestore` |
| Window maximize at real display size | DDR-719 caps at 512×512; lift to real geometry | `smoke-maximize` |
| Pointer resize handles — all edges | DDR-718 covers bottom-right only | `smoke-resizeall` |
| `SURF_EV_CLOSE` notification | Owner saves state before forced close | `smoke-surfclose` |
| Compositor double-map `PTE_SW_SHARED` audit | — | `smoke-sharedpte` |
| OKLab horizon bands / animated mesh | DDR-716 deferred mesh + horizon bands | `smoke-horizon` |
| vDSO callable reader (`vdso_entry.asm`) | ring-3 seqlock reader (IMP-C) | `smoke-vdso-read` |
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
| F#68 metric lockbox e2e | kernel ✅ Python ✅ — e2e wiring unverified | `smoke-lockbox-e2e` |
| F#69 `inventor_agent` | ⬜ not started | `smoke-inventor` |
| F#70 `tournament_agent` | ⬜ not started | `smoke-tournament` |
| F#71 subconscious world model | ⬜ not started | `smoke-worldmodel` |
| F#72 `verifier_agent` | ⬜ not started | `smoke-verifier` |
| F#73 sovereign NL UI | ⬜ not started | `smoke-nlui` |
| F#74 capability discovery | ⬜ not started | `smoke-capdiscovery` |
| F#75 lineage memory | ⬜ not started | `smoke-lineage` |
| F#76 tamper-evident ledger | ⬜ not started | `smoke-ledger` |
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
| **OPEN-2 SMP intermittent fix** | `smoke-resched`, `smoke-blkmq-trace`, `smoke-msixap`, `smoke-crosswake`. Root-cause DDR-863 SMP issues. Active fix. | 20× each |

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

## WHAT "DONE" MEANS — DEADLINE 2026-08-24 23:59 UTC

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
- [ ] All open issues (OPEN-1, OPEN-2, OPEN-10, B#3, FSRM, smoke-agents,
      FAT32 multi-cluster, Dependabot highs) CLOSED
- [ ] x86_64 ISO built and bootable
- [ ] aarch64 ISO built and bootable
- [ ] riscv64 ISO built and bootable
- [ ] `v1.0.0` tagged on `main`
- [ ] Zero open issues, zero warnings, zero unlogged exclusions

**ISO must be testable by 2026-08-24. v1.0.0 tag on main by 2026-08-24 23:59 UTC.**
**After v1.0.0 is tagged: begin Phase 10 (Quantum Layer) immediately.**

**Begin with Phase 1 Item 1 (FSRM fix). Parallelize across groups. Do not stop.**

---

## HYGIENE GATES — must pass before EVERY commit

1. `build/kernel.bin` warning-clean (`-Werror` clang + nasm).
2. `make ci-probe-rodata-check` — no forbidden sentinels.
3. No `[BUG]` lines in serial log.
4. `make smoke-blkmq` exits rc=0.
5. `make ci-shard-check` passes.
6. `make smoke-rqstress-liveness` exits rc=0.
7. `make smoke-blk-integrity` exits rc=0.
8. `make smoke-shell` 5/5 locally.

---

## ORIENTATION

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
