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
Free range: **DDR-1001+** (936-1000 allocated; 999 = multi-arch parity ASSESSMENT (answer: not achievable), 1000 = OPEN-1 DECISION (does not close; E1/E2 named); 998 = SURF_EV_CLOSE, IMPLEMENTED + gated (M3 unmeasured); 997 = resize from any edge, IMPLEMENTED + gated + mutation-checked; 994 = the OPEN-1 route-1 yield-stall detector, IMPLEMENTED + gated; 995 = Alt+Tab, IMPLEMENTED + gated; 996 = TCB freed while queued, FIXED + gated + mutation-checked).
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
| **OPEN-1** | `smoke-surfdestroy` intermittently misses `PRADYOS_SURFDESTROY_CHURN_OK` | **At least one instance is a ring-0 `#PF` — DDR-985.** Measured **19/20** local (kernel `d31b4023b0f74d06` @ `46ece3f`). Run 16: `component: NEXUS isr` / `exception: #PF page fault`, after `[sfs] 64K write/read byte-exact OK`. **NOT the DDR-981 signature** — no `[apfreeze]` and no `compl wait timeout` in any of the 20 runs, so do not read it as a B#3 recurrence. | **STILL OPEN. The lwIP defect is fixed and PROVEN; OPEN-1 is NOT closed by it.** DDR-987/988 fixed a real cross-CPU lwIP use-after-free (`g_net_lock`), and DDR-990's two-CPU hammer POSITIVELY proves that fix: 40,000 connect/close pairs clean on the fixed kernel, `#GP` at `tcp_new_port+0x2d` with `RDI=0xDDDDDDDDDDDDDDDD` in <1000 iterations on the reverted one — mutation-checked both ways. **But DDR-990 §12 established that this does not close OPEN-1.** OPEN-1 is at least THREE signatures: (1) the CI route — a HANG in `sys_read`/`vfs_read` with **no panic at all**, (2) the local `#PF` (1/20, DDR-985), (3) the hammer's `#GP`. The hammer closed route 3, which was never OPEN-1's own artefact, and **no panic-based detector addresses route 1** — a hang prints nothing to detect. A green CI suite is not evidence here and never was: at a ~1/20 base rate a clean 20-run campaign has ~64% power (0.95²⁰ = 0.358), so a clean sweep happens one time in three even if the defect is untouched. **DDR-994 now provides the missing instrument for route 1** (`[yieldstall]`, in `GLOBAL_FORBIDDEN`): `yield()` has 26 call sites; five are ring-3 reachable but `sys_yield` is a bare call, not a wait — the other FOUR are spin-waits and all four are unbounded — `mnt_lock` (`vfs/vfs.c:27`) sits directly on the `vfs_read` path where the captures hang. DDR-981 fixed the interrupt masking INSIDE `yield()` (which is why `[apfreeze]` stopped firing) but never bounded the spin. The detector REPORTS and keeps spinning — §NON-NEGOTIABLE 3 forbids a semantic change without a captured artefact. **Not a fix, and not a claim that `mnt_lock` IS OPEN-1**: if the next occurrence prints no `[yieldstall]` line the hypothesis is refuted, which is itself a real result. Measured denominator: `mnt_lock` turns over ≈255 spins/tick. |
| ~~OPEN-2~~ | ~~`smoke-resched`, `smoke-blkmq-trace`, `smoke-msixap`, `smoke-crosswake` intermittent~~ | **CLOSED — DDR-981**, via B#3. DDR-977 §8.2 had already measured the whole chain in one `smoke-resched` capture (frozen AP → unit 0's MSI-X routed at it → two `compl wait timeout`s → `[blk] multi-inflight FAIL done=0x0` → `[smp] blk integrity FAIL`); DDR-981 names the cause of the freeze and fixes it. These never failed on a scheduler defect and DDR-863 was the wrong lead. | **CLOSED for the block-touching gates.** NOT claimed for `smoke-crosswake`/`smoke-msixap`, which do no block I/O and could fail for their own reasons — the same reservation DDR-977 §8.2 made, kept. `[apfreeze]` is now in `GLOBAL_FORBIDDEN`, so a recurrence names itself instead of hiding in a flake. Reopen on the first `[apfreeze]` line in CI. |
| **OPEN-10** | `btree churn FAIL` during unrelated SMP gates | **ROOT-CAUSED — the create-then-init race, DDR-964.** `rc=-1` is `-EPERM` (`EPERM==1`) from `cap_ok(cap, CAP_FS_WRITE)`: `sched_create()` made a thread runnable before its caller minted the capability into `->arg`, so a thread picked early ran with `CAP_NULL`. NOT a separate defect from the row at §CURRENT BUILD STATE — this symptom **is** OPEN-10 and DDR-964 is its fix; the two rows contradicted each other and this one was the stale half. | **FIXED (DDR-964), pending CI promotion evidence.** `smoke-sfs-btree-smp4` stays excluded until greens accumulate. |
| **OPEN-11** | `smoke-sha256`, `smoke-rqstress-liveness` | Scratch LBA 1500 overwrote kernel image | **CLOSED — DDR-831.** Do not revisit. |
| ~~Uninit PID~~ | ~~`AGENT_OOM_KILLED` prints garbage PID~~ | **NOT garbage — it is `AE_TEST_PID` (`0xA37E0000`), the self-test's deliberate sentinel, `#define`d at `aether.c:14`** | **CLOSED as a non-bug, DDR-969.** Do not reopen. |
| ~~FAT32 large-file~~ | ~~`execve` of large musl ELF corrupts~~ | **REFUTED — DDR-973.** The attribution was ADR-024's own hypothesis ("most likely"), never measured, and `read_cluster_chain` has never existed in this repo — the reader is `fat32_read`. `run /CMUSL.ELF` (30,488 B = 60 clusters) execve's clean. `/BIG8K.TXT` (16 clusters) and `/EXECTEST.ELF` (9 clusters) were already read correctly by green gates. | **CLOSED as not-reproduced, and GATED.** `smoke-fat32-multicluster` verifies 65,536 B / 128 clusters byte-for-byte + 6 straddles + the ADR-024 execve case, every run. Mutation-checked (DDR-973 §6). Do not re-root-cause without a `FAT32MC FAIL` artefact. |
| ~~Dependabot~~ | ~~5 alerts (2 high, 3 moderate)~~ | **IDENTIFIED — Dependabot PR #2.** `@hono/node-server` 1.19.14→2.1.0 (GHSA-9mqv-5hh9-4cgg, unauthenticated memory-leak DoS via aborted WebSocket handshake) and `fast-uri` 3.1.2→3.1.5 (GHSA-4c8g-83qw-93j6, GHSA-v2hh-gcrm-f6hx, GHSA-7p8r-x3mc-p8w7) in `/tools/graph_mcp`. Two packages, five advisories = the "5 alerts". | **CLOSED — already remediated.** `package-lock.json` carries **2.1.0** and **3.1.5**, at or above every fix; `npm audit` = 0 vulns at every severity across 97 packages. PR #2 is superseded (base `dev/phase1` @ `fd876cd`, far behind `main`), left open for the operator to close. **PR #3 (ubuntu 24.04→26.04) DECLINED:** not security; the Dockerfile pins 24.04 deliberately so container and WSL builds agree, and changing the whole toolchain under 149 gates days before the deadline reintroduces the drift the image exists to remove. Revisit post-1.0. Also fixed: `dependabot.yml` npm `directory` was `/` (no package.json there) → `/tools/graph_mcp`, + a `github-actions` ecosystem. |
| **OPEN-13** | `[kheap] double-free ptr=… objsize=0x80` → `*** KHEAP PANIC: kfree: double free ***` at t≈247 | **UNKNOWN — one capture, DDR-980 §2.** `smoke-blkmq-trace`, shard 4, on a DOCS-ONLY commit, so not a regression. NOT OPEN-2 despite that gate being on its list — different signature; treating it as OPEN-2 would be colour-matching. `KHEAP_DEBUG` is unconditionally 1, so this detector is live in the SHIPPED kernel. | **Cannot name the structure yet.** `objsize=0x80` is a GENERIC kmalloc size class (128), not a dedicated cache (pcb=512, cap=16, ipc=256), so the detector's "size class → structure" mapping does not resolve — any `kmalloc(65..128)` qualifies. Narrowing needs alloc/free return addresses recorded per object. **DDR-986 designs the instrument and corrects this row on two points.** (1) The missing datum is the **first free's** return address, not the allocation's — the panic already stands at the second free, where `__builtin_return_address(0)` is free. (2) "touches a hot allocator path, so make it opt-in" does not hold: `cache_free` (`kheap.c:129`) already walks the slab free list — O(`free_count`), up to 31 entries for the 128 class — plus a 128-byte `memset`, on EVERY `kfree`, unconditionally under `KHEAP_DEBUG`. One 8-byte store is noise beside that, and opt-in would guarantee the instrument is OFF in CI, the only place OPEN-13 has ever appeared. |
| **OPEN-12** | `*** NEXUS KERNEL PANIC *** / component: NEXUS isr` at t~185, shard 0 | **UNKNOWN — ring-0 exception, one CI occurrence (run 32595646699, `b43d6b0`).** NOT a regression: that commit's only kernel change is inside `if ((now % 500) == 0)` and the log has no `[hb]` line, so it never ran; the same SHA's sibling matrix run PASSED; and 10/10 local runs are clean. | **ROOT CAUSE CANDIDATE FOUND — DDR-996, and it is the first READABLE capture.** A second occurrence (run 32702096039, shard 2, `smoke-blk-integrity`, `f74e5c5`) carried an intact register block because DDR-979's stream merge worked: `RIP=fair_candidate+0x3A`, `RAX=0xDEADBEEFDEADBEEF` (`PMM_POISON`), `q->head` itself poisoned. Cause: `sched_exit` leaves a thread linked on its per-CPU runqueue, and BOTH reap paths unlink only the all-threads ring before `kfree` — so a TCB reaped before `rq_take` popped it was freed while a queue still pointed at it. Fixed by unlinking in `sched_free_tcb`; 16/16 victims measured, mutation-checked. **NOT yet closed:** OPEN-12's ORIGINAL capture lost its RIP to the very interleaving DDR-979 fixed, so its faulting address is unknown and identity is unproven — matching on `component:` alone is colour-matching. Closes on a clean campaign, not on the fix. Old note kept below. |

> **(prior) CANNOT DIAGNOSE YET — DDR-979.** The `exception:`/`vector=`/`RIP=` block was overwritten by make's stderr interleaving mid-line in the job log. `run_shard.sh` now merges the streams (`2>&1`) so the next occurrence is readable. Do NOT guess from `component: NEXUS isr` — every non-recoverable ring-0 vector prints it. **UPDATE 2026-08-23 (DDR-985): now locally reproducible via `smoke-surfdestroy`, 1/20.** The exception type DIFFERS from the CI capture — that was `#GP` (0x0D), this is `#PF`. Two defects, or one corruption producing varied faults: **not established**. |
| ~~B#3 / DDR-806~~ | ~~`-smp 4` block I/O returns `-EIO` after a 5 s wait~~ | **ROOT-CAUSED AND FIXED — DDR-981.** DDR-977 got as far as the mechanism (an AP stops taking its own LAPIC timer interrupt; which AP varies) but not the cause. The cause: `SYSCALL` entry clears IF via `MSR_SFMASK` (`syscall.c:229`) and the entry path deliberately never re-enables it (`syscall_entry.asm:46`), so **every yield-spin reachable from ring 3 spun with interrupts masked** — `mnt_lock` (`vfs.c:27`), both pipe waits and the blocking console read (`sys_io.c:57/268/293`), and `sys_yield`. `context_switch` preserves per-thread RFLAGS, so the mask is carried across the switch: two such threads on one CPU hand off to each other forever and never reach idle's `sti; hlt`. The CPU is not halted or starved — it runs normally with IF clear. An NMI dump settles it in one line: `masked=0 swen=1 isr48=0 irr48=1 tpr=0 if=0` — LVT unmasked, LAPIC enabled, no stuck in-service vector, a timer **pending and undelivered**, and IF the only remaining blocker. virtio-blk and the LAPIC are both innocent. | **CLOSED.** Fix: an interrupt window in `yield()` (the one choke point all five sites share; fixing `sys_yield` alone would not have fixed the observed livelock, which was in `mnt_lock`). **20/20 boots at `-smp 4`: 0 frozen APs, 0 `compl wait timeout` — before: 6/14 boots frozen with 5–11 timeouts each, and 0 timeouts on every unfrozen boot.** `ymask` ≈ 6.1M/boot is the denominator (R17). Mutation-checked: removing the fix reddens `smoke-blk-integrity` on the first run, named by `[apfreeze]`. |
| ~~smoke-smpuser B#3~~ | ~~`[smp] user on AP OK` never appears~~ | **NOT REPRODUCED 2026-08-22.** `smoke-smpuser` passes at `QEMU_SMP=4`, and `[smp] user on AP OK` is present in every captured boot. | **CLOSED as not-reproduced.** Note the prescribed action was unrunnable anyway: it says to insert `kprintf(...)`, and **`kprintf` does not exist in this kernel** (the console API is `kputs`/`kputdec`). Same defect in `PRADYOS_MASTER_PLAN.md` TASK 4. |

---

## CURRENT BUILD STATE

- **Gate count: 156** assigned across **10** shards, **7** excluded — measured by
  `make ci-shard-check` on 2026-08-25, not carried forward. This line read "149"
  (and, before that, "105"); both were stale. Shard matrix widened 6 -> 10,
  makespan 38.6 -> 20.8 min. Recent additions: 147 -> 148 `smoke-iso-userspace`
  (DDR-972), 149 `smoke-fat32-multicluster` (DDR-973), then `smoke-nethammer`
  (DDR-990), `smoke-modkeys` (DDR-991), `smoke-superkey` (DDR-992) -> 152,
  then `smoke-yieldstall` (DDR-994) -> 153, `smoke-rqfree` (DDR-996) -> 154,
  `smoke-resizeall` (DDR-997) -> 155, `smoke-surfclose` (DDR-998) -> 156.
  **Re-measure rather than increment this** — it has been wrong three times.
- **NSI max: 96** (`SYS_KEY_POLL`, DDR-991). **Next free: 97.** Table size: 128.
  Measured from `kernel/syscall/syscall.h:168-170`. This line previously said 93
  and §INV.14 said 74 — both wrong, and the older note claiming "§INV.14 was
  right" was wrong too. `user/prism.c` ships against 95.
- **`kernel.bin`**: **1,065,350 B** against the 1,572,864 B size gate — 507,514 B
  of headroom (DDR-973's probe costs the page-aligned 8,192 B every embedded probe does; DDR-981's NMI probe costs 4,104 B). The old "~545 KiB, 768 KiB ceiling" was stale in both terms.
- **DDR free range: DDR-1001+** (936-1000 allocated; 999 = multi-arch parity assessment, 1000 = OPEN-1 decision; 998 = SURF_EV_CLOSE ask-then-force, IMPLEMENTED+gated+M1b/M2-mutation-checked (M3 unmeasured); 985 = OPEN-1 refutation, 986 = OPEN-13 instrument, 987 = lwIP core lock, 988 = lwIP deferred work, 989 = vruntime sampling starvation, 990 = net hammer probe BUILT+mutation-checked, 991 = PS/2 modifiers + NSI 96, 992 = Super+M chord, 993 = modifier aggregate DERIVED, 994 = OPEN-1 route-1 detector IMPLEMENTED+gated, 995 = Alt+Tab rebind IMPLEMENTED+gated+mutation-checked, 996 = TCB freed while queued FIXED+gated, 997 = resize from any edge IMPLEMENTED+gated+mutation-checked). **This file carries the free range in TWO places (§INV.4 and here) and they have disagreed before — update both.**
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
- **OPEN-1 is NOT closed.** 19/20; the one failure is a ring-0 `#PF` (DDR-985).

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
| SMEP / SMAP | `CLAC`/`STAC` around every copyin/copyout | `smoke-smep` |
| Kernel W^X identity-alias removal | `vmm_protect_kernel()` — remove identity alias (DDR-757 residual) | `smoke-wx` |
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
| PRISM RX line discipline / echo / readline | — | `smoke-readline` |
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
| Ctrl+Alt+T | Launch PRISM terminal window | `smoke-ctrlaltt` |
| Per-window restore from dock | DDR-717 restores all; add per-tile | `smoke-perrestore` |
| Window maximize at real display size | DDR-719 caps at 512×512; lift to real geometry | `smoke-maximize` |
| ~~Pointer resize handles — all edges~~ | **DONE — DDR-997.** Eight regions, 14 px; SE unchanged bit-for-bit. A W/N drag needs a MOVE *and* a resize through two non-atomic syscalls — move first, and clamp the size BEFORE deriving the origin (clamping after leaves the fixed edge sliding). Mutation-checked M1/M2/M3, three distinct kernel hashes; M3 fails `smoke-drag` because BETA's published `dg=` sits inside ALPHA's east band. Also fixed here: `PRADYOS_WM_GEOM` was republished only on a surface-count or focus change, so it was stale after any move or resize. | `smoke-resizeall` ✅ |
| ~~`SURF_EV_CLOSE` notification~~ | **DONE — DDR-998.** Event type 4 (1/2/3 were already resize/scroll/composited). The compositor ASKS, then forces after a bounded grace; the owner may delay, never veto. A surface id does not identify a surface — 16 slots recycle immediately — so `struct surface` gained a generation counter, and `surf_take_free`'s whole-struct wipe had to be taught to preserve it. M1b/M2 mutation-checked on distinct hashes and they fail DIFFERENT arms; M3 (recycle guard) recorded UNMEASURED with its reason. | `smoke-surfclose` ✅ |
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
