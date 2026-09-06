# PRADYOS — BUILD TRACKER

**Live dashboard, single source of truth for status. Update in the SAME commit
as any status-changing code. If this drifts from the tree it is a defect — five
instances of exactly that failure are recorded in §4.**

Every claim below is anchored to a verified artefact: a file that exists, a gate
that ran, or a CI job that concluded. Where something is assumed rather than
verified, it says so.

---

## 1. Last verified state

| | |
|---|---|
| **`main`** | **`3b4830a`** — PROMOTED on 3 consecutive greens (30804476970, 30811210244, 30811221820). Carries DDR-817 sharding, X25519, SHA-512, DDR-822/823. |
| **`dev/phase1`** | `bef93c2` — **NOT promoted**. Tip before it (`fd876cd`) went green twice: runs `30878361148`, `30879247169`. |
| **Verified** | 2026-08-04, tree clean |
| **NSI max** | **78** — `SYS_ACC_SEAL` 77 / `SYS_ACC_OPEN` 78 **registered and linked** (DDR-827 raised the load window). Next free **79**. Table size **128**. |
| **CI gates** | **122** assigned across 6 shards · **5** excluded, each with a stated reason |
| **CI health** | 8 reds on 2026-08-03 fully audited (DDR-828): 5 × `smoke-ed25519` (expected, DDR-826), 7 × `smoke-syscallfuzz` (stale 60 s window — fixed), 1 × `smoke-resched`, 1 × `smoke-blkmq-trace` (single occurrences, triaged to OPEN-2). |
| **CI wall-clock** | ~25 min (was 2 h 08 m before DDR-817) |

**CI evidence on `dev/phase1`:**

| run | tip | verdict |
|---|---|---|
| `30804476970` | `3b4830a` | **GREEN — smoke-x25519 CI-PROVEN on its first CI run** |
| `30811210244`, `30811221820` | `3b4830a` | dispatched for the 3-green promotion rule |
| `30773609553` | `1fa8495` | GREEN |
| `30773828417` | `e4bb576` | in flight at last check |
| `30774291748` | `1e40464` | in flight at last check |

**Stale feature branches — do not delete, do not merge.** `feature/arm64`,
`feature/arm64-grace`, `feature/apple`, `feature/rv64` all sit at `a398b62`.
Landing zones for later arch work, not live.

---

## 2. Tracker corrections made this session

The previously-circulated verification table was wrong on load-bearing points.
Each corrected entry below was re-checked against the tree, not assumed.

| Stale claim | Verified reality |
|---|---|
| "X25519 ❌ Not started — no source in `kernel/crypto/`" | **`kernel/crypto/x25519.{c,h}` exists** (10,468 B). All RFC 7748 vectors pass on host, incl. a constant-independent commutativity check and small-order rejection. Gate **now passes 4/4 on a clean host** and is registered in shard 3 — the earlier failures were host contamination plus a stale artefact (DDR-822/823). |
| "SHA-512 not present" | **`kernel/crypto/sha512.{c,h}` exists.** 4 FIPS 180-4 vectors, **gate `smoke-sha512` A/B-verified**, in shard 3. |
| "`arch/aarch64`, `arch/riscv64` — zero source files" | **Both populated**: `boot.S` + `start.c` + `kernel.ld` (~278 lines total). `make kernel-<arch>` builds; `smoke-<arch>` boots under QEMU. **Both green in CI every run.** |
| "ISO pipeline — 3–5 sessions, from-scratch ports needed" | **Reframed.** Two of four targets already reach their boot banner. ISO work is *packaging* (GRUB/EFI/OpenSBI wrapping) on top of working boots, not porting. |
| "OPEN-10 — add a spinlock to `sfs.c`" | **No such target.** `kernel/fs/sfs/sfs.c` has **zero** global mutable state; the VFS already serialises per-mount via an atomic sleep-mutex (`kernel/fs/vfs/vfs.c:25`, DDR-locks-3). See §5. |
| "`tools/boot_test.sh`" | Actual path `tools/qemu_runner/boot_test.sh`. |
| "109 CI gates" | 119 assigned, 5 excluded. |

**Scope note on the arch ports, stated so it is not over-read:** ADR-034 scopes
them as **boot-only**. They reach `kmain` and print a banner. No drivers, no FS,
no userspace. That is real and CI-proven, and it is not feature parity.

---

## 3. Phase summary

Counts carried from the Aug 2 assessment **except** Phase 0 and Phase 6, which
were re-verified this session (see §2).

| Phase | Name | Items | ✅ | ⚠️ | ❌ | % |
|---|---|---|---|---|---|---|
| 0 | Toolchain & Build | 11 | 6 | 2 | 3 | **55%** ↑ |
| 1 | Bootloader | 9 | 6 | 2 | 1 | 67% |
| 2 | NEXUS Kernel Core | 57 | 46 | 5 | 4 | 81% |
| 3 | Driver Framework | 33 | 24 | 1 | 5 | 73% |
| 4 | Filesystem Layer | 25 | 22 | 0 | 0 | 88% |
| 5 | Userspace Foundation | 40 | 31 | 2 | 2 | 78% |
| 6 | Crypto Stack | 9 | 5 | 2 | 0 | **61%** ↑ (+2 🔒) |
| 7 | AETHER Agent Runtime | 50 | 41 | 8 | 1 | 82% |
| 8 | Sovereign Desktop | 30 | 27 | 1 | 2 | 90% |
| 9 | Assembly Optimization | 7 | 1 | 2 | 4 | 14% |
| 10 | ~~Quantum Layer~~ | 0 | 0 | 0 | 0 | **WITHDRAWN 2026-09-02** — operator decision (PR #17, Part A): quantum *hardware* integration is a speculative future-research note, not a backlog item. Remote cloud API with queue-time latency is incompatible with microsecond kernel scheduling. Nothing was ever built toward it, so nothing is removed. **Post-quantum *cryptography* (ML-KEM/ML-DSA) is the opposite: mandatory v1 scope, before the ISO** — CLAUDE.md §PHASE 3. |
| **Total** | | **286** | **217** | **25** | **28** | **76%** |

Phase 0 rose because 0.2/0.3 (aarch64/riscv64 cross-compile) are ⚠️→✅ for the
compile+boot scope ADR-034 defines. Phase 6 rose because X25519 (code) and
SHA-512 (code + gate) were mis-recorded as absent.

### Phase 6 — Crypto Stack, itemised (most-contested table)

| # | Feature | Status | Evidence |
|---|---|---|---|
| 6.1 | SHA-256 (DDR-811) | ✅ | `smoke-sha256`, 4 FIPS vectors |
| 6.2 | Metric lockbox (DDR-812) | ✅ | `smoke-lockbox`, `smoke-metric` |
| 6.3 | HMAC + HKDF (DDR-818) | ✅ | `smoke-hkdf`, 3 RFC 5869 vectors |
| 6.4 | ChaCha20-Poly1305 (DDR-819) | ✅ | `smoke-aead` **PASSES**, registered in shard 4. First run failed on a wrong recalled nonce (§2.3.2 vs §2.4.2) — the probe, not the primitive. |
| 6.5 | **SHA-512** (DDR-821) | ✅ | `smoke-sha512`, A/B-verified, shard 3 |
| 6.6 | **X25519** (DDR-820) | ✅ | `smoke-x25519` 4/4 on a clean host, registered in shard 3 |
| 6.7 | **Ed25519** (DDR-821) | ✅ | `smoke-ed25519` **PASSES** — `PRADYOS_ED25519_VECTORS_OK`. All RFC 8032 §7.1 vectors + tamper/wrong-key/non-canonical-S rejection. The earlier failure was DDR-826 (writable global in an R+X-only probe), not the arithmetic. |
| 6.8 | **ACC** (DDR-813) | ✅ | `smoke-acc` **PASSES in CI** — `PASS smoke-acc (151s)` in the fully green run 30944847959 on `93ceee7`. The OPEN-11 suspicion against its introducing commit is resolved: the cause was DDR-831's stale scratch LBA, not ACC. Upgraded ⚠️→✅ only after CI concluded, not before. |
| 6.9 | **AGS — Agent Goal Signing** (DDR-814) | ✅ | NSI 79 `SYS_GOAL_SIGN` (CAP_SOVEREIGN) / 80 `SYS_GOAL_VERIFY` (CAP_AGENT) — the inverse split to ACC's, because signing *authorises* a goal. `PASS smoke-ags (120s)` in the fully green run 30960084022 on `9fb8ea4`; 20/20 locally beforehand. Its first CI run went red — I had inserted the new audit codes mid-enum, renumbering wire-format values the ring-3 probes hardcode (DDR-832), and the verification rebuild silently no-opped because kernel headers were not prerequisites (DDR-833). Both root-fixed with executable checks. |
| 6.10 | **ACC session rotation** (DDR-815) | ✅ | NSI 81 `SYS_ACC_ROTATE`, CAP_SOVEREIGN. Per-agent channel table keyed by the agent's Ed25519 verify key (authenticated, unlike a recyclable pid); rotation REVOKES by raising the replay floor past any issuable sequence. `smoke-acc-rotate` 4 arms — `PASS smoke-acc-rotate (120s)` in the fully green run 30966987476 on `7be4b65`; 20/20 locally beforehand. |
| 6.11 | **Secure credential vault** (DDR-834) | ✅ | NSI 87 `SYS_VAULT_PUT` / 91 `SYS_VAULT_GET`, both CAP_SOVEREIGN. SFS-backed `/VAULT.BIN`, ChaCha20-Poly1305 under HKDF(`PRADYOS-VAULT-v1`), **record name as AAD** so a record cannot be renamed on disk while its tag still verifies. `smoke-vault` 4 arms incl. on-disk tamper rejection — `PASS smoke-vault (120s)` in the fully green run 30993915008 on `b2e7836`; 20/20 locally beforehand. |
| E.1 | **Agent memory** (DDR-836) | ✅ | NSI 82 `SYS_MEMORY_WRITE` / 83 `SYS_MEMORY_READ`, `CAP_MEMORY` (1<<18) + `tcb.is_memory`. **Shared blackboard, NOT per-agent isolated** — the capability is the whole boundary; pid-keyed isolation was rejected because pids recycle. `smoke-agentmem` 4 arms — `PASS smoke-agentmem (120s)` in the fully green run 31003118400 on `7f7a9d3`; 20/20 locally beforehand. |
| E.2 | **Agent checkpoint / resume** (DDR-837) | ✅ | NSI 84 `SYS_CHECKPOINT_AGENT` / 85 `SYS_RESUME_AGENT`, both CAP_SOVEREIGN. The target blocks **itself** at its next syscall boundary — setting THREAD_BLOCKED from another CPU races the scheduler for a thread that may be RUNNING. Guards: init and self cannot be frozen; unknown pid is `-ESRCH`. `smoke-checkpoint` asserts on observed ps state, never on elapsed time. `PASS smoke-checkpoint (120s)` in the fully green run 31015668039 on `35bab14`; 20/20 locally beforehand. |
| E.3 | **Spawn-depth cap** (DDR-838) | ✅ | `SPAWN_DEPTH_MAX = 3`. Keyed on **lineage** (`tcb.agent_depth`), not on `is_agent` — fork does not inherit authority flags, so a cap on `is_agent` would be escaped by a single fork. `SYS_FORK` returns `-EAGAIN` (a ceiling, not a permission error) at depth 3. Scoped to agent lineages, so ordinary process trees are untouched — verified `smoke-shell` 5/5. `PASS smoke-spawndepth (120s)` and `PASS smoke-shell (60s)` in the fully green run 31028810861 on `83a761a`; 20/20 locally beforehand. |
| E.4 | **DAG action queue** (DDR-839) | ✅ | NSI 92 `SYS_SUBMIT_CHILD_ACTION` + `parent_action_id`. A child cannot be APPROVED before its parent. Cycles are **structurally impossible** (ids monotonic, parent must pre-exist), so no cycle check exists. A separate syscall rather than NSI 31's unused 4th register, which is undefined in every existing caller. `PASS smoke-actiondag (120s)` in the fully green run 31043474501 on `362cb36`; 20/20 locally beforehand. |
| 6.9 | AGS (DDR-814) | 🔒 | needs 6.7 |

---

## 4. Open defects

| ID | Symptom | Cause / hypothesis | Status |
|---|---|---|---|
| **OPEN-1** | `smoke-surfdestroy` intermittently misses its sentinel | ring-0 `#PF` panic in an ISR (DDR-985); 19/20 local | open, **ACTIVE — has a local reproducer** |
| ~~OPEN-11~~ **CLOSED** | `smoke-sha256` (and `smoke-rqstress-liveness`) failed after the first run on a fresh image | **ROOT-CAUSED AND FIXED — DDR-831.** `blk_selftest` wrote its scratch sector at a hardcoded LBA 1500, chosen when the kernel was capped at 512 sectors; DDR-827 grew the kernel to ~1666 sectors, so the self-test wrote *into the kernel image*, and QEMU persisted it — corrupting the probe ELFs in `.rodata` for every later boot | **CLOSED — CONFIRMED BY CI.** Run 30944847959 on `93ceee7`: all 11 jobs green, with `PASS smoke-sha256 (90s)` and `PASS smoke-rqstress-liveness (180s)` actually executed. The build guard printed `kernel ends at LBA 1666, scratch sector 4095 — clear` — 1666 > the old literal 1500, direct confirmation of the overlap. Locally 20/20 + 5/5. |
| ~~**OPEN-2**~~ | ~~historical intermittent CI reds~~ | **CLOSED 2026-08-22 — DDR-981**, as downstream of B#3. DDR-863's observation was right and its inference wrong: all four gates do run `QEMU_SMP=4`, but the shared cause is not "SMP timing" — it is that `yield()` spun with `RFLAGS.IF` clear, so a CPU running two yield-spinning ring-3 threads stopped taking interrupts entirely and never serviced the block completions those gates wait on. DDR-977 §8.2 measured the full chain in one `smoke-resched` capture. | **CLOSED for the block-touching gates.** NOT claimed for `smoke-crosswake`/`smoke-msixap`, which do no block I/O — reservation kept from DDR-977 §8.2. `[apfreeze]` is now in `GLOBAL_FORBIDDEN`, so a recurrence is a named failure rather than a flake. |
| **OPEN-9** | `smoke-shell` fails locally, passes CI, identical binary | **leaked QEMU holding the image write-lock** | **misattribution FIXED (DDR-823)**; **pre-flight guard + orphan CAUSE fixed (DDR-951)** — `INT`/`TERM` trap reaps our own QEMU on cancellation, which is where orphans came from; pre-flight `pgrep -f "[q]emu…"` now exits 3 before burning the window. Root cause still not caught live on a `smoke-shell` failure |
| **OPEN-10** | `'btree churn FAIL'` during unrelated `-smp 4` gates | **ROOT-CAUSED + FIXED — DDR-964.** The narrowing below stands (`rc=-1` is `-EPERM` from the `cap_ok(cap, CAP_FS_WRITE)` branch in `vfs_create`, with all three DDR-884 candidates eliminated by the value); the cause behind it is a **create-then-init race** — `sched_create()` made a thread runnable before its caller had minted the capability into `->arg`, so a thread that got picked early ran with `CAP_NULL`. Fixed by `sched_create_blocked()` → set arg → `sched_unblock()` at 8 sites in `main.c`. Reproduced on demand for the first time, which is what distinguishes this from the earlier narrowing. | **fix landed, pending CI promotion evidence** |
| **ISO not self-contained** | `make iso` produces a bootable image that reaches `NEXUS KERNEL OK` and then idles with `[blk] no block device` / `[fs] no mountable filesystem found` — no PRISM, no aetherd, no compositor, no networking | The ISO carries only `pradyos.img` + `esp.img`; the root filesystems are separate virtio-blk disks the gates attach and the ISO omits. Post-handoff the kernel speaks only virtio-blk/AHCI/NVMe, and the CD is ATAPI (`disks=0`). No ramdisk/initrd facility exists. | **OPEN — blocks v1.0.0. DDR-971.** Control arm proves the kernel is fine. Fix needs a root reachable after handoff: ramdisk `blk_device` behind `blk_register()` (recommended), stage-2 second payload, or ATAPI+ISO9660. |
| ~~**B#3 / DDR-806**~~ | ~~`-smp 4` virtio-blk completion stall~~ | **CLOSED 2026-08-22 — DDR-981.** Not virtio-blk, not the LAPIC, not "timer/IRQ delivery under SMP": `SYSCALL` entry clears IF via `MSR_SFMASK` and never restores it, so every yield-spin reachable from ring 3 spun masked, and `context_switch` carried the mask across switches until the CPU never took another interrupt. Fixed with an interrupt window in `yield()`. 20/20 at `-smp 4`, 0 `compl wait timeout`; mutation-checked. | ✅ |
| **smoke-x25519** | ~~probe does not reach its sentinel~~ | was host contamination + a stale artefact (DDR-822/823) | **RESOLVED — 4/4 clean-host passes, now in shard 3.** Unblocks Ed25519 → ACC → AGS once green in `main` |
| OPEN-7 | per-boot probe selection | — | CLOSED (DDR-804) |
| OPEN-8 | console input loss | — | CLOSED (DDR-809) |

### The recurring structural defect — SIXTEEN instances

One bug in many costumes: **a check that discards or absorbs invalid input
instead of rejecting it, so drift is silent and looks like success.**

| # | Where | Silent drop | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran in CI | DDR-817 — `make ci-shard-check` |
| 2 | Makefile user sources | 14/31 probes never rebuilt ⇒ gates tested stale binaries | DDR-822 — `$(wildcard user/*.c)` |
| 3 | `user/` `_start` attribute | a new probe silently reintroduces a #GP | DDR-823 — `make ci-start-align-check` |
| 4 | `syscall_register()` | `num >= MAX_SYSCALLS` discarded; NSI 80+ would vanish | DDR-823 — panic + table 80→128 |
| 5 | `check_global_forbidden()` | printed only matching lines, **discarding the `op=` line that names the defect** | DDR-824 — 40 lines of context |
| 6 | **crypto sources + Makefile not prerequisites** | **a build that reports success and never runs** — DDR-822 fixed `user/` and stopped there | DDR-825 — glob `kernel/crypto/*` and list `Makefile` |
| 7 | **writable global in an R+X-only probe** | **link succeeds; the FIRST STORE faults at runtime**, and the gate reports it as a missing sentinel i.e. "the crypto is wrong" | DDR-826 — `make ci-probe-rodata-check` |

| 8 | PMM double-free | a freed frame re-entered the pool | DDR-830 |
| 9 | mid-enum insertion | shipped wire format silently renumbered | DDR-832 — append-only + `_Static_assert` |
| 10 | kernel headers/sources not prerequisites | same class as #2/#6, third recurrence | DDR-833, DDR-835 — recursive wildcards |
| 11 | **SkillOpt accepting a TIE** | **each tie changes the skill with no evidence it helped; every step "passes" and the skill drifts somewhere nobody chose** | DDR-847 — `>` not `>=`, with its own rejection branch |
| 12 | **a skill revision thinning its own refusals** | **every refusal is a task the agent declines and scores zero on, so removing one RAISES the score — the optimiser is rewarded for it** | DDR-848 — refusal count may rise, may not fall |

| 13 | `rates.get(model, 0.0)` for cost | **an unpriced model charged as free.** The total stays plausible and the report looks complete, so the number used to decide "is this agent worth running" is wrong in the direction that says keep going | DDR-849 — `UnknownModel` raised; zero must be *declared* |
| 14 | **`check_invariant()` asserting a TAUTOLOGY** | `available` is derived by subtraction, so the accounting identity over it **could never fail**. It read as a strong invariant and was decorative — the defect appearing inside code written to guard against the defect | DDR-849 — assert the independently-tracked counters instead |

| 15 | **building from the tracker LABEL instead of the spec text** | four items (#47/#48/#50/#52) shipped satisfying their one-line titles while missing what §3D actually specified — and it **passes review**, because the label is what a reviewer checks against | DDR-850 — corrected against `AETHER_MASTER_FEATURES.md` §3D |

| 16 | **the MUTATION HARNESS itself** | a mutation whose target string was absent was **skipped with a warning** while the run still printed kills -- from the *previous* mutation's stale `__pycache__`. The defect inside the tool built to detect the defect | DDR-853 -- `tools/mutation/mutate.py` aborts on a missing/ambiguous target, clears bytecode, and **fails if a mutation kills nothing** |

**Rule earned: when a check discards input rather than rejecting it, the discard
must be loud.** And: **a tracker line is a label FOR a requirement, not the
requirement — build from the spec text.** Instances 11 and 12 extend it — the invalid case an optimiser
will find is the one that scores well, so it needs an explicit branch and its
own rejection message, never a fall-through comparison.

---

## 5. OPEN-10 — RESOLVED AS A MISNOMER by DDR-880 (2026-08-09)

> **OPEN-10 is not a B+tree bug and not a distinct defect.** It is the item-47
> lost-thread failure seen through a different sentinel. Measured 2/30 on
> `smoke-sfs-btree-smp4`; in both failures the kernel printed **neither**
> `[sfs] btree churn OK` **nor** `... FAIL` — the probe never ran, because
> `fs_test_thread` is lost before reaching it. `[boot-stamp] B` is absent in
> every captured failure of both gates.
>
> The B#3 / virtio-blk hypothesis below is **closed**: DDR-878 cleared the block
> layer (block gates 0/8, the slot-wait precondition witness never fires, and
> the statement after the stall point is an embedded ELF load with no disk I/O).
>
> The 90 s-window explanation below was right about the timeouts it measured and
> does not cover these: `smoke-sfs-btree-smp4` already runs at `TIMEOUT_S=180`,
> and a thread lost at t≈240 is not waiting for more time.
>
> Text below kept as written.

## 5. OPEN-10 — superseded diagnosis (kept for history)

**Signature (the only thing that counts as OPEN-10 data):** the probe prints the
literal string `btree churn FAIL`.

**Occurrences:** 2, both CI, both `QEMU_SMP=4` gates — `smoke-smp` (shard 5) and
`smoke-rqstress` (shard 3), consecutive runs.

**Local stress produced NO evidence either way.** 20 × `smoke-sfs-btree` at
`-smp 4`: 16 failed, **0 with the OPEN-10 signature** — all 16 were timeouts
(`required pattern not found`). Counting those as OPEN-10 would be
colour-matching, which this project has already paid for once. The 90 s window
is the likely cause: three extra vCPUs multiply TCG work without adding host
parallelism. **Confirmed** — the same probe at `TIMEOUT_S=180` under `-smp 4`
**passed**.

**The queue's prescribed fix has no target.** `kernel/fs/sfs/sfs.c` contains
zero global mutable state, and every VFS operation is already serialised
per-mount by an atomic sleep-mutex. There is nothing to add a spinlock to.

**Live hypothesis — OPEN-10 is B#3 seen through the SFS probe.** The churn probe
does 40 × (create + 64 KiB write + unlink), i.e. heavy block I/O; both hits were
`-smp 4`; B#3 is a known `-smp 4` virtio-blk completion stall. A lost completion
makes `vfs_write` return ≠ 65536, which the probe reports as `op=write`. **If
true, fixing B#3 fixes both.**

**How it gets confirmed or refuted:** DDR-824 made the harness print 40 lines of
context on a global-forbidden hit. The probe writes
`[sfs] churn FAIL op=<create|write|unlink> iter=<N>` immediately before the
summary line, and that line previously never reached CI output. **The next
occurrence will name its failing operation.** `op=write` supports the
unification; `op=create` or `op=unlink` refutes it.

`smoke-sfs-btree-smp4` exists as an on-demand reproduction surface, **excluded**
from the shard matrix — registering it now would make CI red on a known-open
defect and block unrelated promotions.

---

## 6. Work queue — complete, dependency-ordered

Status key: ✅ done · 🔵 in progress · ⬜ not started · 🔒 blocked

### Immediate

| TASK | Item | Status |
|---|---|---|
| 0 | BUILD_TRACKER tip SHA | ✅ |
| 1 | OPEN-10 + CI promotion | 🔵 CI green on `1fa8495`; DDR-824 diagnostic landed; root cause open |
| 2 | `smoke-x25519` clean-host re-verify | ✅ 4/4, exclusion removed, in shard 3 |
| 3 | SHA-512 gate | ✅ A/B verified, shard 3 |
| 4 | DDR-821 Ed25519 | 🔒 rule 7 — needs TASK 2 green in `main` |
| 5 | DDR-813 ACC (NSI 77/78) | 🔒 needs 4 + `smoke-aead` |
| 6 | DDR-814 AGS (NSI 79/80) | 🔒 needs 4 |
| 7 | DDR-815 ACC rotation (NSI 81) | 🔒 needs 5 |
| 8 | ~~B#3 `-smp 4` virtio-blk stall~~ | ✅ **DONE — DDR-981** (and it was never a virtio-blk stall) |

### Group 1 — tracker & build-system integrity (x86_64 v1.0.0)

| # | Item | Status |
|---|---|---|
| 1 | Tracker contradictions + NSI 87 collision | ✅ DDR-840; CI run 31053809587 on `f80efa6` |
| 2 | Docker reproducible build env | ✅ `Dockerfile` + `make docker-build`; `ci-docker-check` asserts the base tag is pinned and no unpinned archive is added. **Scope: pins the ENVIRONMENT, not bit-for-bit output** — that needs `SOURCE_DATE_EPOCH` work, not claimed here |
| 3 | CMake/Makefile hybrid | ✅ **RESOLVED BY SUBSTITUTION (DDR-843)** — skipped for v1.0.0; the stale-prerequisite class CMake would have prevented is already closed structurally by the build wildcards. Revisit post-1.0 with the arch ports |
| 4 | VirtualBox runner | ✅ `tools/vbox_runner/run_vbox.sh`; `ci-vbox-check` parses it and asserts it exits **77** (not 0) when VirtualBox is absent. Real D.2 boot is operator-run |
| 5 | x86_64 chipset variant coverage | ✅ `smoke-chipset` — q35/qemu64, **pc/qemu64 (i440FX)**, q35/Nehalem, **q35/Opteron_G5 (AMD)**. 20/20 local |

#### Item 3 assessment — recommendation: SKIP CMake for v1.0.0, revisit post-1.0

CMake earns its keep on multi-platform generation, multi-consumer library
targets, and IDE integration. None applies to x86_64 v1.0.0: one architecture,
one toolchain (clang/lld/nasm), no external consumers of the build.

The defect CMake would plausibly have prevented is stale-prerequisite builds —
and that class (DDR-822/825/833/835) is now closed structurally by the
`KERNEL_ALL_CS` + `KERNEL_HS` + `USER_ALL_SRCS` wildcards, which cannot go stale
the way a hand-maintained list did.

Against that, porting ~2,400 Makefile lines carrying 130 gate recipes is a
rewrite of the exact machinery that proves the release, and it would re-open the
staleness class we spent four DDRs closing.

**The honest case FOR CMake** is the deferred arch ports: when `arch/aarch64` and
`arch/riscv64` become real, per-arch toolchain files are where CMake pays. That
is post-1.0 by the operator's own scoping, so the payoff is post-1.0 too.

**Decision taken in DDR-843: skipped for v1.0.0, revisit post-1.0.**

### Group 2 — Section E / capability / agent core close-out

| # | Item | Status |
|---|---|---|
| 6 | NSI 86 `SYS_APPROVE_CODE_REWRITE` + `CAP_REWRITE` (1<<21) | ✅ `PASS smoke-coderewrite (120s)`, run 31094358972 on `93b51ea` |
| 7 | Audit hash chain + NSI 93 `SYS_VERIFY_AUDIT` | ✅ `PASS smoke-auditchain (120s)` + `PASS smoke-auditchain-tamper (120s)`, run 31094358972 |
| 8 | Section 3C action types | ✅ **8 of 14** CI-green in the same run; 6 blocked with reasons below, 1 of which is an open operator decision |
| 9 | Section 3D daemon features (#45-65) | ⬜ not started |
| 10 | Section F #66-76 beyond item 7 | ⬜ not started |
| 11 | Section G 12-agent roster | ⬜ not started |
| 12 | J-01..J-06 retrospective audit | ✅ **all six verified — DDR-845**. J-01 pytest green in CI's `aether-layer` job (`-W error`); J-02 I-01..I-10 modules present and in the pytest tree; J-03 the kernel chain is pure C and never touches Python `hashlib` — but there are TWO audit chains and the naming invites confusion; J-04 Python S1-S14 and kernel S1-S8 collide in label only and must never merge; J-05 covered by the shipped `smoke-privacy-netfilter` gate; J-06 cloud bridge gated off pending R1/R3, which independently confirms the DDR-843 `ACTION_BROWSE_WEB` deferral. **No code written — correct for an audit** |
| 13 | `smoke-invariants` S1-S8 | ✅ `PASS smoke-invariants (120s)`, run 31104672684 on `81a3eaf`; 20/20 local. Covers S1,S2,S4,S5,S6,S8. **S3/S7 deliberately NOT claimed** — they depend on unbuilt F#66-72, and a green arm for an unbuilt subsystem would convert "not implemented" into "verified". S5's no-erase-path half is asserted at build time by `ci-audit-noerase-check` |
| 14 | ChaCha20-Poly1305 gate-wiring verification | ✅ **resolved by inspection**: `smoke-aead` exists and is CI-registered (shard 4, 90 s), testing RFC 8439 vectors directly. It is not merely exercised via `smoke-vault` — it is its own gate. No new gate needed |

#### Item 8 — the six NOT built, with reasons (not silently dropped)

| action type | why not |
|---|---|
| `ACTION_CAPTURE_FRAME` | post-L7 per spec; needs `CAP_SCENE` + camera path |
| `ACTION_SCAN_ENVIRONMENT` | post-L7; SLAM3R, no hardware path |
| `ACTION_QUERY_SCENE` | post-L7 NL query over a scene graph that does not exist |
| `ACTION_PARSE_DOCUMENT` | needs a 64 MiB local OCR model; no model-shipping path |
| `ACTION_EXEC_CODE` | needs a sandboxed interpreter — a subsystem, not an action |
| `ACTION_BROWSE_WEB` | needs a headless browser **and network egress = cloud bridge (DDR-793)**. **DEFERRED post-1.0 (DDR-843)** — nothing in the release path depends on it, and enabling it is a security-posture change (outbound egress from an agent-capable OS), not a feature toggle. Needs an explicit instruction to enable the bridge |

Declaring these as enum values without enforcement would be worse than omitting
them: an agent could submit one and the kernel would queue an action nothing
implements. They are omitted until their subsystem exists.

### Section E — kernel syscalls (TASK 9)

**NSI allocation lives in DDR-840, not here.** This table carries STATUS only;
restating numbers in two places is what produced the 87 collision it records.

| Item | NSI | Status | Evidence |
|---|---|---|---|
| `SYS_MEMORY_WRITE` / `SYS_MEMORY_READ` | 82/83 | ✅ | `PASS smoke-agentmem (120s)`, run 31003118400 on `7f7a9d3` (DDR-836) |
| `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` | 84/85 | ✅ | `PASS smoke-checkpoint (120s)`, run 31015668039 on `35bab14` (DDR-837) |
| `spawn_depth` cap in TCB | — | ✅ | `PASS smoke-spawndepth (120s)`, run 31028810861 on `83a761a` (DDR-838) |
| DAG action queue (`parent_action_id`) | 92 | ✅ | `PASS smoke-actiondag (120s)`, run 31043474501 on `362cb36` (DDR-839) |
| `SYS_APPROVE_CODE_REWRITE` | 86 | ✅ | DDR-842. `CAP_REWRITE` + `CAP_SOVEREIGN`, neither alone. `PASS smoke-coderewrite (120s)`, run 31094358972; 20/20 local |
| `SYS_VERIFY_AUDIT` (chain verify, F#76) | **93** | ✅ | DDR-842. Renamed from `SYS_READ_AUDIT`: that name is **already NSI 37**, shipped, and three probes parse its struct — widening it would have overflowed their buffers. 93 verifies, never returns records. `PASS smoke-auditchain` + `PASS smoke-auditchain-tamper` (120s each), run 31094358972; 20/20 local |

### Section 3B — capability bits (TASK 10)

| bit | capability | status |
|---|---|---|
| 1<<18 | `CAP_MEMORY` | ✅ shipped — `tcb.is_memory`, gate `smoke-agentmem` (DDR-836) |
| 1<<19 | `CAP_OCR` | ⬜ deferred post-1.0 (no OCR path in the x86_64 v1 scope) |
| 1<<20 | `CAP_EXEC` | ⬜ deferred post-1.0 |
| 1<<21 | `CAP_REWRITE` | ⬜ **Group 2 item 6** — required by NSI 86, always CAP_SOVEREIGN co-approved |
| 1<<22 | `CAP_SCENE` | ⬜ deferred post-1.0 (post-L7) |
| 1<<23 | `CAP_NET_BROWSE` | ⬜ deferred post-1.0 |

Each shipped bit needs an enforcement gate; a capability with no gate is a
comment. Bits 19/20/22/23 are defined but unwired only when their consume
lands — an unused bit position costs nothing, and reserving them now prevents a
mid-bitmask insertion later (the DDR-832 hazard applied to capabilities).

### Section 3C — action types #31–44 (TASK 11) — all ⬜

`ACTION_READ_FILE` · `DELETE_FILE` · `EXEC_CODE` · `SEND_IPC` ·
`PARSE_DOCUMENT` · `BROWSE_WEB` · `QUERY_MEMORY` · `CAPTURE_FRAME` ·
`SCAN_ENVIRONMENT` · `QUERY_SCENE` · `REWRITE_AGENT_CODE` ·
`PROPOSE_HYPOTHESIS` · `RUN_EXPERIMENT` · `EVOLVE_GENOME`

### Section 3D — ring-3 / daemon #45–65 (TASK 12) — **21 of 21 COMPLETE**

| # | item | status |
|---|---|---|
| 1 (#45) | skill.md — 8 roster files, validated | ✅ DDR-846 |
| 2 (#46) | SkillOpt loop | ✅ DDR-847 |
| 3 (#47) | SkillOpt-Sleep — harvest→mine→replay→consolidate, pauses agents | ✅ DDR-848 + **DDR-850** |
| 4 (#48) | skill-update validation — `CAP_SOVEREIGN` always | ✅ DDR-848 + **DDR-850** |
| 5 (#49) | multi-agent transfer | ✅ DDR-848 |
| 6 (#50) | TokenJuice — context compression ≤80% + hard token ceiling | ✅ DDR-849 + **DDR-850** |
| 7 (#51) | JSONL trajectory log | ✅ DDR-849 |
| 8 (#52) | cost accounting — `token_count` + `latency_ms` | ✅ DDR-849 + **DDR-850** |

| 9 (#53) | `goals.md` per agent — `CAP_SOVEREIGN`, checkable criteria | ✅ DDR-851 |
| 10 (#54) | subconscious loop — period, goal-diff, ≤25% of 60 syscall/s, idle-only | ✅ DDR-851 |
| 11 (#55) | MOSS pipeline — staging, regression gate, snapshot rollback, co-approval | ✅ DDR-851 |

| 12 (#56) | OCR→memory — confidence quarantine, mandatory provenance | ✅ DDR-852 |
| 13 (#57) | multi-modal context builder | ✅ DDR-852 |
| 14 (#58) | privacy mode (ring-3) — fails closed; kernel remains the enforcement | ✅ DDR-852 |
| 15 (#59) | model routing — privacy-gated, deterministic, budget-aware | ✅ DDR-852 |

| 16 (#60) | hypothesis tree — versioned, persists; wraps **D-07**'s `Hypothesis`, not a rival type (DDR-855) | OK DDR-853+855 |
| 17 (#61) | `genome.md` — lineage archived, rationale required | OK DDR-853 |
| 19 (#63) | dead-end registry | OK — **D-13 `FailureMemoryRegistry`**, which already existed; DDR-855 added the missing divergence score to it rather than shipping the duplicate DDR-853 wrote |

| 18 (#62) | vector knowledge graph — online learning, refuses to evict | OK DDR-856 |
| 20 (#64) | population tournament — unranked is not last, ties do not promote | OK DDR-856 |
| 21 (#65) | replayable run visualiser — deterministic, self-contained, never un-redacts | OK DDR-856 |

**Section 3D is complete: all 21 items (#45-#65).**

16 hypothesis tree · 17 genome.md · 18 vector knowledge graph ·
19 dead-end registry · 20 population tournament · 21 run visualiser — all ⬜

### Section F — visionary #66–76 (TASK 13)

| # | Feature | Status |
|---|---|---|
| F#66 | architect_agent | ⬜ |
| F#67 | healer_agent | ⬜ |
| **F#68** | **metric lockbox** | ⚠️ kernel (DDR-812) ✅ + Python ✅; **end-to-end wiring unverified** — `smoke-lockbox-e2e` ⬜ |
| F#69 | inventor_agent | ⬜ |
| F#70 | tournament_agent | ⬜ |
| F#71 | subconscious world model | ⬜ |
| F#72 | verifier_agent | ⬜ |
| F#73 | sovereign NL UI | ⬜ |
| F#74 | capability discovery | ⬜ |
| F#75 | lineage memory | ⬜ |
| F#76 | tamper-evident ledger | ⬜ |

### Section G — 12-agent roster (TASK 14)

8 kernel slots (KRYOS…SOLIN) ✅ registered + UI cards ✅; **skill prompts ✅ for
all 8** (DDR-846 — this line previously read "❌ for all 8" and was one commit
stale). DDR-846's design decision: the 8 legacy names **become** the 8
highest-priority Section G roles rather than sitting alongside them, so the
12-agent roster extends one working set instead of creating a second.

| Section G role | slot | status |
|---|---|---|
| file_agent | KRYOS | ✅ skill.md, spawnable |
| shell_agent | PRAX | ✅ skill.md, **not yet spawnable** (CAP_EXEC unwired) |
| research_agent | LUMYN | ✅ skill.md, **not yet spawnable** (CAP_NET_BROWSE unwired) |
| ocr_agent | AHNIS | ✅ skill.md, **not yet spawnable** (CAP_OCR unwired) |
| vision_agent | IRIS | ✅ skill.md, **not yet spawnable** (CAP_SCENE unwired) |
| healer_agent | RUFLO | ✅ skill.md, **not yet spawnable** |
| orchestrator_agent | HERMES | ✅ skill.md, spawnable |
| verifier_agent | SOLIN | ✅ skill.md, spawnable |

Remaining 4 of the 12 (subconscious/ai_scientist/architect/tournament) — **⬜**;
they have no kernel roster slot, so they need one before a skill file means
anything. Kernel-side per-persona dispatch is still future for all 12.

### J-01…J-06 retro audit (TASK 15) — ✅ ALL SIX VERIFIED (DDR-845)

### Section B remaining (TASK 16)

B#1 NVMe IRQ ⏸ · B#4 SFS default root ⬜ · B#6 ext4 write ⬜ · B#9 I/O APIC ⬜ ·
B#10 NUMA affinity ⬜ · B#12 PRISM job control ⬜ (`$?` ✅, SIGPIPE ✅) ·
B#13 dynamic linker ⬜ · B#14 NAS scheduler ⬜ · B#15 PMM policy ⬜

### TASK 17 — ISO pipeline

| Target | Boot status | ISO status |
|---|---|---|
| x86_64 | ✅ boots, 118 gates | ⬜ multiboot2 + grub-mkrescue |
| aarch64 | ✅ **boots in CI** | ⬜ EFI/U-Boot packaging |
| riscv64 | ✅ **boots in CI** | ⬜ OpenSBI + U-Boot packaging |
| Apple Silicon | ⬜ | ⬜ m1n1 shim over the aarch64 kernel |

### TASK 18–21

18 `prad` package manager (NSI 87–89 — **renumber, 87 is taken by
`SYS_READ_AUDIT`; use 88–90**) ⬜ · 19 Phase 9 assembly ⬜ ·
20 security invariant gates S1–S8 ⬜ · 21 v1.0.0 release ⬜

---

## 7. August 31 — honest assessment

**28 days. ~69 features remaining** (down from 73: SHA-512 gated, X25519 and the
arch bootstraps recorded correctly).

### Risk flags

1. **The observed rate does not reach 69 features in 28 days.** The last three
   sessions produced roughly 2 features each plus five infrastructure fixes. The
   infrastructure was necessary — without DDR-822 every local verification was
   potentially against a stale binary — but it is not feature throughput.
2. **Five silent-drop defects in three sessions implies more exist.** Every one
   was found while chasing something else, not by looking, and each made some
   past "verified" claim weaker than it read.
3. **The remaining work is not the easy remainder.** It is disproportionately
   never-started items (Sections 3C/3D/F/G, `prad`, Phase 9, invariant gates)
   with no scaffolding.
4. **One thing got cheaper.** The ISO task is packaging over two already-booting
   arch targets, not four ports.

The honest read: **all 286 items by Aug 31 is not achievable at the observed
rate.** Achievable is a defensible x86_64 build with the crypto chain closed,
both `-smp 4` races fixed, and ISOs for the three targets that already boot.
Which subset ships is a scope decision, not an engineering one.

## Item 3 — CMake/Makefile hybrid: OPERATOR ASSESSMENT BRIEF (recommend SKIP)

Required by the directive before this item may be skipped. Not implemented.

**Recommendation: SKIP.** Rationale, in the order that matters:

1. **No correctness benefit.** The existing Makefile already enforces every
   invariant the build must hold: `-Werror` for clang *and* nasm, the 256 KiB
   per-ELF `EXEC_MAX` budget check, `ci-probe-rodata-check`, `ci-shard-check`
   (144 gates across 6 shards), `ci-start-align-check`, and the DDR-831 scratch
   LBA check. A hybrid adds no invariant that is not already enforced.

2. **It adds a second source of truth.** Two build descriptions of one tree is
   the same failure class as a hand-maintained list mirroring a directory
   (DDR-822/825): the copies drift, and the drift is silent. The project has
   already paid for that class of bug more than once.

3. **The measured pain point is not build description.** Seven tooling defects
   have cost real sessions this cycle, and every one was a *shell escaping* or
   *stale artifact* problem (WSL inline `$((…))`, inline `$(md5sum …)`, stale
   mtime after `git stash pop`, a corrupt `kernel.elf` surviving a failed link
   while `make` reported success). CMake addresses none of these; the
   mitigations that did address them are `ab_rate.sh`, `check_hash.sh`, and
   Rules 19/22/23/24.

4. **Cost is not zero.** Every gate recipe, shard entry, and probe-embedding
   rule would need a second expression, and each becomes a place for the two to
   disagree.

**What would change this recommendation:** a concrete requirement the Makefile
cannot express — cross-compiler toolchain selection for the aarch64/riscv64 ISO
packaging (Item 13-B/13-C), if that turns out to need per-target toolchain
files. That is the one place to revisit. It is not a reason to convert now.

**Status: DEFERRED — awaiting operator sign-off. Do not implement without an
explicit approval message.**

## Group 2 close-out — CI verification (Items 4-A/4-B/4-C)

| gate | shard | excluded | CI status |
|---|---|---|---|
| `smoke-coderewrite` | 0 | no | passing in full-suite greens |
| `smoke-auditchain` | 1 | no | passing in full-suite greens |
| `smoke-auditchain-tamper` | 2 | no | passing in full-suite greens |

All three are registered in `tools/ci/gate_shards.txt` and absent from the
`shard_check.sh` EXCLUDE list, so RULE 4's "in the shard matrix" is satisfied.
A failing gate fails its whole shard, so the two most recent full-suite CI
greens (`0253fbe`, `fbb868f`) are direct evidence that all three passed in both
runs. No separate re-run is needed to establish CI confirmation.

## FAT32 multi-cluster reads — closed as a refutation (DDR-973, 2026-08-22)

The Group B item "FAT32 multi-cluster read fix — `execve` of large musl-C ELF
corrupts — root-cause via `read_cluster_chain`" is closed **without a code
change to `fat32_read`**, because there is no defect to fix.

| what the backlog said | what was measured |
|---|---|
| the bug is in `read_cluster_chain` | that function has never existed in this repo; the reader is `fat32_read` (`kernel/fs/fat32/fat32.c:309`) |
| `execve` of a large musl-C ELF from FAT32 corrupts | `run /CMUSL.ELF` — 30,488 B = **60 clusters** — execve'd clean, printed `PRADYOS_MUSL_OK`, exited 0 |
| multi-cluster reads are broken | `/BIG8K.TXT` (16 clusters) has been read byte-exact by `smoke-shell` through a pipe, and `/EXECTEST.ELF` (9 clusters) through the full `sys_execve` path, for months |

The source of the claim is ADR-024 §D5, which hedged it at the time ("root cause
is **most likely** FAT32 multi-cluster reads"). The hedge was dropped as the item
was copied forward into `build_status.md` and then `CLAUDE.md`, where it became a
named function to repair. ADR-024 now carries an addendum saying so.

**Shipped instead:** `smoke-fat32-multicluster` (shard 3, 90 s) + probe
`user/fat32mctest.c` + fixture `/BIGPAT.BIN` (64 KiB = 128 clusters). Three arms:
a byte-exact scan of all 65,536 bytes, 6 cluster-boundary straddles, and
ADR-024's own execve case. Mutation-checked both ways (DDR-973 §6) — including
one mutant that **passed the first cut of the gate**, because the specified
`(7n+3)&0xFF` pattern has period 256 and every 512-byte cluster on the volume
was therefore identical. The shipped pattern is `(7n + 3 + 31*(n>>8)) & 0xFF`.

**Still deferred (unchanged, and unblocked by the above):** init-driven
`fork`+`execve` **respawn** of PRISM. That is unbuilt work. PRISM's `run`
builtin itself is not disabled and never was.

## smoke-agents — not reproduced since instrumentation (DDR-968, 2026-08-22)

`[DEFERRED: not reproduced]`. The `PRADYOS_AGENT_WITNESS_WAIT pid= disp= state=
n=` instrument has been live since `ea4601e` and has never printed — by design,
it emits only while the witness is unarmed, so a green boot produces none of it.
`smoke-agents` is gating (shard 2, absent from the `shard_check.sh` EXCLUDE
list), so a recurrence would fail its whole check suite; 18 suites have been
green on shard 2 since. With no artefact there is no named mechanism, and
§NON-NEGOTIABLE 3 forbids a speculative fix. The instrument stays armed and the
item reopens on the first witness line: `disp=0` would confirm DDR-968 §2's
reading (agent thread exists, never switched in), `disp>0` would refute it.

## Dependabot triage (STEP 4, 2026-08-22) — COMPLETE

The alerts turned out to be readable after all, from the wrong direction: this
session has no Dependabot-alert API tool, but Dependabot's own **open pull
requests** name every package and advisory. Both were found and triaged.

### PR #2 — the security alerts. Already remediated; the PR is stale.

`chore(deps): Bump the npm_and_yarn group across 1 directory with 2 updates`,
opened 2026-08-04 against `dev/phase1`, targeting `/tools/graph_mcp`:

| package | PR asks | advisories it cites | lockfile today |
|---|---|---|---|
| `@hono/node-server` | 1.19.14 → 2.1.0 | GHSA-9mqv-5hh9-4cgg — unauthenticated memory-leak DoS via an aborted WebSocket handshake (`upgradeWebSocket`; a missing/malformed `Sec-WebSocket-Key` leaked the `IncomingMessage` and left a promise pending). Fixed in 2.0.10. | **2.1.0** ✅ |
| `fast-uri` | 3.1.2 → 3.1.5 | GHSA-4c8g-83qw-93j6 (3.1.3), GHSA-v2hh-gcrm-f6hx (3.1.4), GHSA-7p8r-x3mc-p8w7 (3.1.5 — literal backslash accepted in the URI authority) | **3.1.5** ✅ |

Two packages, five advisories between them — which is where "5 alerts (2 high,
3 moderate)" comes from. **`tools/graph_mcp/package-lock.json` already carries
versions at or above every fix**, which is exactly why `npm audit` reports
`{info:0, low:0, moderate:0, high:0, critical:0}` across all 97 packages. The
remediation landed in the lockfile independently of the PR; PR #2 is superseded
and its base (`dev/phase1` @ `fd876cd`) is far behind `main`.

**Left open rather than closed.** Closing another actor's PR on the operator's
repository is their call, not this session's, and nothing depends on it.

### PR #3 — `ubuntu` 24.04 → 26.04. Declined, with a reason.

Not a security update — a plain docker version bump. **It should not be merged**,
because it contradicts the Dockerfile's own stated purpose:

> The base is pinned to the same distro the project already builds on
> (Ubuntu 24.04, per CLAUDE.md), so container and WSL builds agree.

The image exists to remove toolchain drift between the container and the WSL
host. Moving the container to 26.04 while the host stays on 24.04 reintroduces
precisely the drift it was built to eliminate, and would do so five days from
the deadline with a full toolchain change (clang, lld, nasm, QEMU) underneath a
147-gate suite. Revisit after v1.0.0, together with the WSL environment.

### Config defect found and fixed on the way

`.github/dependabot.yml` pointed its npm ecosystem at `directory: "/"`, where
there is no `package.json` — `directory:` is a literal path, not a glob, so npm
**version** updates had never scanned anything. Now `/tools/graph_mcp`. A
`github-actions` ecosystem was added too: the workflows pin `actions/checkout@v5`
and `actions/setup-python@v5` by major tag, which silently absorbs whatever the
action owner pushes to that tag, and nothing was watching it.

This did not affect the alerts above: **security** updates come from the
repository dependency graph and fire regardless of that file, which is why PR #2
existed at all despite the broken path. The two are separate systems.

---

## 2026-08-22 — B#3 and OPEN-2 closed (DDR-981)

`yield()` spun with `RFLAGS.IF` clear. `SYSCALL` entry masks interrupts via
`MSR_SFMASK` (`syscall.c:229`) and the entry path deliberately never re-enables
them (`syscall_entry.asm:46`: *"No nesting: SFMASK clears IF, so a syscall is
never interrupted"*), so every yield-spin loop reachable from ring 3 was a
masked spin: `mnt_lock` (`vfs.c:27`), both pipe waits and the blocking console
read (`sys_io.c:57/268/293`) — PRISM's own read loop — and `sys_yield`.
`context_switch` preserves per-thread RFLAGS, so two such threads on one CPU
hand off to each other forever and never reach the idle loop's `sti; hlt`. That
CPU then runs normally with interrupts off: its LAPIC timer tick stops, and any
virtio-blk completion MSI-X routed at it is never serviced.

Named by an NMI probe (NMI being the one interrupt that still reaches a CPU with
IF clear). The dump refutes three of DDR-977 §5's four candidates and confirms
the fourth in one line: `masked=0 swen=1 isr48=0 irr48=1 tpr=0 if=0`.

Fix: an interrupt window in `yield()` — the single choke point all five call
sites share. Fixing `sys_yield` alone would not have worked; the threads in the
capture were in `mnt_lock` under `vfs_read`.

Result at `-smp 4`: **20/20 boots, 0 frozen APs, 0 `compl wait timeout`**
(before: 6/14 boots frozen, 5–11 timeouts each, 0 on every unfrozen boot).
Denominator `ymask` ≈ 6.1M masked yields per boot. Mutation-checked: removing
the fix reddens `smoke-blk-integrity` on the first run.

Gate lesson worth keeping: `smoke-smp` and `smoke-rqstress` both measured 20/20
at `-smp 4` while this defect was live. The gates did not catch it; the evidence
sat in serial logs nobody asserted on. `[apfreeze]` is now in `GLOBAL_FORBIDDEN`
so that class of silent failure is a named red.

`docs/AETHER_MASTER_FEATURES.md` is deliberately unchanged: neither B#3 nor
OPEN-2 is a feature it tracks.

---

## DDR-991/992/993 — Group E input foundation, and a review that found what the gates could not

**Shipped:** `SYS_KEY_POLL` (NSI 96), extended-scancode decode, modifier
tracking, Super+M sovereign toggle, and the paired-modifier fix.
**Gates:** `smoke-modkeys` (six ring-3 arms + one kernel arm), `smoke-superkey`.

DDR-991 lifted two limits in the DDR-703 driver that had to move together: the
missing `0xE0` case (an extended key's prefix has bit 7 set, so it was swallowed
as a break code and the following byte decoded as an unprefixed make — right-Ctrl
delivered a bare left-Ctrl) and the `sc >= 0x40` cap that dropped every function
key and arrow. DDR-992 added the Super+M toggle and, with it, the rule that a
chord must not also deliver text on NSI 46.

**DDR-993 is the part worth reading.** CodeRabbit's review of PR #14 found a real
defect neither DDR's gate could see: `mods_set` wrote the aggregate bit
unconditionally, so releasing one side of a paired modifier cleared it while the
other side was still held. For Shift that is a wrong glyph. For Ctrl/Alt/Meta it
disables DDR-992's chord suppression — so **a chord starts typing text again**,
the exact regression DDR-992 existed to prevent, one commit after it landed.

Two lessons, both measured rather than argued:

1. **DDR-991 §6 claimed arm E was "the arm that matters most … a latched-modifier
   regression passes every other arm here."** True of the regression it imagined
   (a modifier stuck down), false of the one that shipped. Arm E presses one Ctrl
   key, so the buggy line and the correct line agree. The mutation check passed
   against a defect it could not express. **Measured:** both DDR-993 mutants
   still print `PRADYOS_MODKEYS_OK` — all six ring-3 arms pass on a broken
   kernel.

2. **The missing arm was not unwritten, it was UNWRITABLE.** QEMU's HMP `sendkey`
   emits a press and its release as one indivisible action; two keys of a pair
   held at once is not a sequence it can express. The fix was structural: split
   the decode from the port read (`ps2kbd_feed`), and assert it in ring 0.

Fix: only the eight physical keys carry state; the `KMOD_*` aggregate is
**recomputed** from them on every edge, so it cannot disagree with its sides by
construction. Separately, `key_ev.code` now comes from the unshifted map — it had
carried the shifted glyph, so one physical key's make and break disagreed
whenever Shift was released between them.

Mutation-checked both ways: M1 (aggregate reverted) fails at selftest step 3,
M2 (code identity reverted) at step 11, gate exit 2 in both cases; three distinct
kernel hashes so no run measured a stale binary.

`docs/AETHER_MASTER_FEATURES.md` is deliberately unchanged: the input driver is
Layer 7 plumbing, not an AETHER feature it tracks.

---

## DDR-994 — the instrument for OPEN-1 route 1

**Shipped:** `yield_stall_note()`, three instrumented yield-spins, `smoke-yieldstall`
(shard 9), `[yieldstall]` in `GLOBAL_FORBIDDEN`. Gate count 152 -> 153.

OPEN-1 route 1 is a **hang with no panic**, and every instrument this repo had was
keyed to something being printed or something faulting. `[apfreeze]` (DDR-981)
triggers on "this cpu stopped taking interrupts"; in route 1 the cpu is fine —
`g_ticks` advances, other threads run, one thread waits forever — so that
detector is structurally blind to it.

`yield()` has 26 call sites. Five are ring-3 reachable; `sys_yield`
(`syscall.c:155`) is a bare call that returns immediately, not a wait. The other
**four are spin-waits and all four are unbounded**. DDR-981 fixed the interrupt masking *inside* `yield()` but never
bounded the spin. `mnt_lock` (`vfs/vfs.c:27`) sits directly on the `vfs_read`
path where the captures hang.

**Three sites instrumented, one deliberately not.** The console read
(`sys_io.c:293`) waits for a keystroke and is *legitimately* unbounded — PRISM
sits in it for the whole of every boot. A duration-based watchdog would fire in
all 153 gates on day one and be switched off within the day. The discriminator
is what is waited on, not how long: three sites wait on state owned by another
thread in this system; the console waits on the outside world. That asymmetry is
also why this cannot be a hook inside `yield()`, which does not know its caller.

**It reports, it does not repair.** There is a named mechanism but no captured
artefact of it firing, so §NON-NEGOTIABLE 3 forbids changing locking semantics.
Bailing out of `mnt_lock` on a deadline would turn a hang into a silent `-EIO` on
a live mount — it would look like a fix and destroy the evidence.

**Mutation-checked, and the mutants taught something.** Both M1 (call removed
from `mnt_lock`) and M2 (threshold raised past the arm) kill arm B and leave arm
A green. That asymmetry is the design working — arm A unit-tests the reporter,
arm B tests the wiring — and it is exactly the vacuity the two-arm structure was
written against: **an instrument wired to nothing still passes arm A.** DDR-994
§6 had predicted M2 would fail both arms; that prediction was wrong about its own
design and is corrected in place.

**First measured denominator for a yield-spin:** `mnt_lock` turns over
≈127,344 spins / 500 ticks ≈ **255 spins per tick**.

---

## DDR-1007 — Window maximize at real display size (Group E)

**DONE.** `smoke-wmmax` green for the new reason; M1/M2 mutation-checked on
distinct kernel hashes. Kernel `92eb02028af0a929`.

Maximize asked for **512×512** on a **1024×768** screen — 26% of the display.
It now fills a mode-aware **work area**: 798×728 in Sovereign (clearing the 6 px
accent bar and the 210 px agent panel), the menu bar and taskbar in Manual.
**4.3× the area.**

**The blocker was never the compositor.** 512 was `SURFACE_DIM_MAX`, and
`SURFACE_VA_SLOT` was pinned to it *undocumented* at exactly 512×512×4 = 1 MiB.
`sys_surface_map` maps `s->npages` pages at `BASE + id*SLOT` with **no check that
npages fits the slot**, so raising the dimension cap alone would have mapped
surface *N*'s buffer across surface *N+1*'s window — inside the compositor, which
maps surfaces owned by **other processes**. A cross-window data leak that panics
nothing and trips no sentinel. §INV.13's PT_HI lesson in a new place.

Three limits are now explicit (`SURFACE_DIM_MAX`, `SURFACE_BYTES_MAX`,
`SURFACE_VA_SLOT`) and tied by `_Static_assert`. **M2 proves the tie fires:**
putting the VA slot back to 1 MiB is a *build* error, not a runtime mystery.

**The gate stopped hardcoding geometry (§INV.5).** It reads `w`/`h` out of the
compositor's own `PRADYOS_WM_MAX id=1 w=798 h=728` and asserts the client's ack
matches. **M1 shows this is strictly stronger:** a compositor that publishes 798
and requests 512 satisfied the old `grep "w=512 h=512"` and fails now.

Unmeasured, and recorded as such: the Manual-mode work-area arm (the gate boots
Sovereign), the predicted 2.2× per-window blit cost, and `smoke-wmmax`'s
DDR-975 §8 intermittency — untouched and not claimed fixed.

---

## DDR-1008 — Per-window restore from a dock (Group E)

**DONE.** `smoke-perrestore` (shard 4, fast). DDR-717 shipped minimize with one
restore-**all** keystroke; the dock restores one window at a time.

A strip of tiles along the bottom, one per minimized window, drawn **over** the
windows and present only while `g_min_mask != 0`. Tiles are ordered by ascending
surface id, **not** z-order — `SYS_SURFACE_POLL` is z-sorted and reshuffles on
every raise, which is bad UI and an untestable target (DDR-910's finding). The
dock is an overlay and deliberately does **not** shrink DDR-1007's work area, or
a maximized window would resize itself whenever an unrelated window was
minimized. Sovereign-only: Manual already draws window buttons in its own
taskbar, so wiring those is a separate change, recorded as not done.

**The gate minimizes TWO windows and restores ONE.** The obvious one-window
version is vacuous — DDR-717's `g_min_mask = 0` passes it. The load-bearing
assertion is the last: after restoring BETA, the dock must still publish exactly
one tile and it must be ALPHA (`PRADYOS_WM_DOCK n=1 id=0 title=ALPHA`); a
restore-all implementation publishes `n=0`.

**Found by reading, not by a failing run:** publication could not be folded into
the block that emits `PRADYOS_WM_GEOM`, which is guarded on
`ns != composited || cur_focus != last_focus || geom_moved`. Minimizing changes
none of the three, so a dock line emitted there would never appear after a
minimize — the only moment it matters.

`mouse_inject.sh` gained one backward-compatible variable, `GEOM_LINE`
(default `PRADYOS_WM_GEOM`), so a gate can resolve coordinates out of the dock
line instead. §INV.5 holds: no gate hardcodes a pixel.

---

## DDR-1009 — 1 CI suite in 4 fails on the release candidate; one failure names a mechanism

**THE MEASUREMENT IS THE HEADLINE.** Every commit from `d0a85b5` to `93a4a1f`,
and `fa29506`, is Markdown-only — verified by `git diff --name-only` and by
rebuilding `bb9c6187a30bb0dd` bit-for-bit in a clean worktree. Twelve CI
suite-runs therefore share **one kernel binary**: **9 green, 3 failed**, at four
gates with four signatures (`smoke-smpuser` timeout; `smoke-msixap`
panic-then-hang; `smoke-nethammer` timeout at 20/20; `smoke-smppreempt`
`[apfreeze]`). **§NON-NEGOTIABLE 1 is satisfiable by luck at 25%** — `0.75³ ≈
42%` — and this kernel has already passed it **twice**.

**The fix.** DDR-970 force-releases `g_line_lock` on the panic path. `kputs` —
which the panic printer itself calls — takes `g_console_lock`, a *different*
lock. Nothing released it. DDR-979's one-winner latch then made it worse: the
losing CPU halts in `for(;;) cli; hlt` still holding whatever it held. Artefact:
`smoke-msixap` printed `*** NEXUS KERNEL PANIC ***` and **not one further byte**
for ~100 s until `timeout` killed QEMU, with `idt.c:702` the very next statement.
`console_line_force_release` → `console_panic_force_release`, releasing both.
Ruled out on the way: `kputc`'s UART wait is bounded (`CONSOLE_THRE_MAX`), so it
cannot hang.

**The detector gap.** `*** NEXUS KERNEL PANIC ***` had one emitter and **zero**
consumers — no gate grepped for it, so a ring-0 panic was caught only when it
happened to break an assertion or run out a clock. Added to `GLOBAL_FORBIDDEN`
(now 71 entries). Note the §NON-NEGOTIABLE 6 verification command keys on the
**last** list entry, so appending breaks it; CLAUDE.md was updated in the same
commit and now says so explicitly.

**NOT claimed:** that the lock fix explains the other three signatures (they
produced no panic banner at all), nor that the four are four separate defects,
nor that OPEN-1 route 1 is closed — though the `SYSFSTAT OK` stopping point
recurring *with a panic* does show DDR-994's "route 1 prints nothing" framing is
too strong.

---

## DDR-1010 — OPEN-2 reproduced locally; the cause is a broken SWAPGS discipline

**The campaign DDR-1006 prescribed returned a clean null, and it was the wrong
gate.** `smoke-smppreempt` measured **20/20 PASS** on `bb9c6187a30bb0dd`, zero
`[apfreeze]`. That null was pre-registered as meaning "CI-only" — but it had only
**19% power** (`0.92²⁰ ≈ 0.19` against CI's observed ≈0.08/run), and
**`smoke-blk-integrity` reproduces the defect locally, ~1 in 4.**

**The primary event is not the scheduler.** Four lines before the freeze:
`[percpu] gs FAIL (syscall ctx)` and `[percpu] current FAIL (syscall ctx)` — the
DDR-SMP-3a probe reporting a broken SWAPGS discipline at a **ring-3 syscall
entry**. `current_thread` then resolves into ROM (`pid=0xF000F053`), a later
`sys_mmap` → `vmm_map_in` → `map_core` `#GP`s on it, and the CPU wedges in
`isr_dispatch` with `if=0`, ticks frozen at 186 while the BSP reaches 17500. The
frozen AP's block completions time out, producing the
`[smp] blk integrity FAIL reference-read` that every previous OPEN-2
investigation started from.

Backtrace resolves fully — `isr_dispatch <- isr_common.gs_kernel_in <- map_core
<- vmm_map_in <- sys_mmap` — at the **same RIP** as DDR-1006's CI capture but
from a **different caller**, so the wedge point is `isr_dispatch`, not the timer
path DDR-1006 inferred.

**Detector gap closed.** `GLOBAL_FORBIDDEN` carried `percpu FAIL`, which does not
match either printed string. Only `smoke-swapgs` noticed. Both are now their own
entries (71 → 73).

**Not a regression from DDR-1008/1009**, on mechanism rather than counts: the RIP
matches a capture taken on `bb9c6187a30bb0dd` before either change existed, the
failing path is untouched by the diff, and the boot failed on its own
pre-existing sentinel. The A/B counts (1/4 vs 0/6) are explicitly recorded as
settling nothing, p ≈ 0.40.

**Source defect NOT named. No fix attempted.** Next instrument in DDR-1010 §7:
make the SWAPGS probe continuous (it is one-shot on the first `sys_getpid`),
record the CPU index, then campaign `smoke-blk-integrity` at N ≥ 36.

### DDR-1010 addendum — the continuous SWAPGS probe, and two corrections

**Instrument built (§7).** `gs_discipline_check()` runs at the top of
`syscall_dispatch`, **before** anything dereferences `current_thread` — which
matters, because the rate-limit check on the next line is itself a
`current_thread->is_agent` read, and on the DDR-1010 boot that read went through
a GS base of 0 into the real-mode IVT. The always-on cost is `this_cpu()` plus a
compare; `lapic_id()` (an MMIO read) runs only *after* the cheap check has
already failed, so a healthy boot pays nothing. Latched per LAPIC id, because the
percpu index is exactly what is unusable when this fires.

Non-vacuity checked with an `if (0)` mutant so a healthy kernel reports:
`[percpu] gs FAIL (syscall ctx) apic=0 num=6 gs0=0xFFFFFFFF80136D60
want=0xFFFFFFFF80136D60` — `gs0 == want` proves both sides of the cross-reference
resolve, which is the basis of the whole diagnostic.

**Correction 1 (§4).** The claimed detector gap was not real: `GLOBAL_FORBIDDEN`
already carried a bare `gs FAIL`, so every `boot_test.sh` gate caught the primary
event. Retracted in place.

**Correction 2 (§8), and it is the real gap.** `smoke-shell` never calls
`boot_test.sh` and so applied **none** of the 73 sentinels — while being the gate
CLAUDE.md requires 5/5 before every push. The mutant kernel above **passed** it.
Fixed by `scan_forbidden.sh`, which refuses to report clean if it recovers fewer
than 60 patterns.

Kernel `4b3181f13b2d76aa`, 1,098,122 B, `-Werror` clean. smoke-swapgs,
smoke-blkmq, smoke-blk-integrity, smoke-rqstress-liveness, smoke-selftest,
smoke-shell, smoke-perrestore all PASS; ci-shard-check OK (157/10/7);
ci-probe-rodata-check OK (61 ELFs).

---

## DDR-1011 — OPEN-1 route 1: the STEP 2 decision

**DECIDED: route 1 is OPEN at the deadline**, and the release notes must say so.
DDR-1011 §4 carries the exact wording. Routes 2 and 3 are closed on measured
evidence (DDR-1000 §9 at 95% power; DDR-990 §9 mutation-proven both ways).

**The merge with OPEN-2 is explicitly refused.** Both captures die inside the
same short stretch of `systest`'s syscall sequence and both panic, which invites
the inference that they are one defect. A healthy boot refutes it: the
DDR-SMP-3a probe is one-shot on the first `sys_getpid`, which lands **after**
`SYSREAD OK` —

```
200: SYSIO EFAULT OK   201: SYSOPEN OK   202: SYSFSTAT OK   203: SYSREAD OK
206: [percpu] gs OK (syscall ctx)
```

— and the route-1 capture died **between `SYSFSTAT OK` and `SYSREAD OK`, before
the probe ever ran.** Its silence about GS is not evidence of anything. The two
stopping points are also two syscalls apart, and OPEN-2's boot never reached
`SYSOPEN`. Matching on "same neighbourhood plus a panic" is the colour-matching
DDR-975 §7, DDR-966, DDR-969 and DDR-973 each had to retract.

**What changed today.** DDR-1010 §7 moved the GS check to every syscall, at the
top of `syscall_dispatch`. A boot dying between `SYSFSTAT` and `SYSREAD` now runs
it on `SYSFSTAT` and everything before. So the next route-1 occurrence
discriminates: a `[percpu] gs FAIL … num=M` line means route 1 IS OPEN-2 and
names the syscall that lost GS; its absence means it is not, and this time the
silence is a measurement. No previous route-1 capture had that property.

---

## DDR-1012 — DAWN/DUSK horizon bands (Group E)

**DONE.** `smoke-horizon` (shard 2, fast), M1 mutation-checked. Kernel
`9623c163cd479043`, 1,102,218 B, `-Werror` clean.

DAWN was the **only** ambiance with no backdrop at all — `render_backdrop`'s arm
was a bare `break` with the comment "motes carry it". It now carries a rose band
at 62% of height; DUSK's sun-bloom at (85%, 90%) now rises out of an amber band
at 88% instead of floating above nothing.

**The gate measures pixels, and that is the point.** `horizon_band` samples the
same centre pixel on entry and exit and the compositor publishes both:

```
[horizon] DAWN pre=18092C post=412546
[horizon] DUSK pre=290E00 post=582C0D
```

M1 (blend loop deleted, sentinel kept, kernel `a2dccf7ad726ed55`) gives
`pre == post` and FAILS — **while `boot_test.sh`'s `EXTRA_SENTINEL` check PASSED
on that same mutant.** A sentinel-only gate, the shape every other Layer-7
backdrop gate uses, would have reported PASS on a compositor that drew nothing.

The first version of the assertion compared the band centre against a row above
it. That is vacuous against `render()`'s per-row vertical gradient (DDR-723) —
two different rows differ whether or not a band was drawn. Replaced before the
gate was ever executed.

**Animation assessed and NOT built** (§5), logged as
`[DEFERRED: animation — cannot be gated inside a 120 s window; costs per-frame
work on a compositor with two open scheduling defects]`. A 120 s nebula drift
does not fit a 120 s gate, and a "two frames 20 s apart differ" assertion passes
on any per-frame noise.

Regression: `smoke-backdrop`, `smoke-ambiance`, `smoke-gradient`,
`smoke-cadence`, `smoke-shell` all PASS; `ci-shard-check` OK (158/10/7);
`ci-probe-rodata-check` OK (61 ELFs).

---

## Pre-approved exceptions — the DEFERRED log CLAUDE.md requires

> **SUPERSEDED IN PART — see `docs/PRE_LAUNCH_CHECKLIST.md` §3.** Three entries
> below are now stale: `ACTION_SEND_IPC` shipped as DDR-1033 (NSI 98/99,
> `smoke-sendipc`), and both blockers named under F#73 are gone (DDR-1027
> windowed terminal, DDR-1032 argv/envp). `ACTION_RUN_EXPERIMENT` is **also now
> stale** — DDR-1034 built it: a bounded integer stack machine with no memory
> opcodes at all, `CAP_EXEC` promoted from an unchecked `#define` to a real
> `RES_EXEC` capability paired with `is_exec` on `struct tcb`, and a **separate**
> kernel-written results ring that does not touch the DDR-812 lockbox. The rows
> are left verbatim because this is a log; the checklist carries the corrections.

CLAUDE.md's §PRE-APPROVED EXCEPTIONS says, of each item: *"add a one-line entry
in `docs/BUILD_TRACKER.md` as `[DEFERRED: reason]`"*. **Seventeen items were
listed and none had been logged.** That is not cosmetic: §WHAT "DONE" MEANS
requires *"All items CI-green or carrying a logged pre-approved exception"* and
*"zero unlogged exclusions"*, so the checklist could not have been honestly
ticked. Logged here verbatim from the table, reasons unchanged.

- Intel HDA audio — `[DEFERRED: deferred, optional — no QEMU HDA path in CI]`
- Wayland/wlroots compositor — `[DEFERRED: superseded by shipped custom C framebuffer compositor]`
- CMake/Makefile hybrid — `[DEFERRED: deferred post-1.0, awaiting operator sign-off (DDR-843)]`
- Apple Silicon / m1n1 — `[DEFERRED: deferred post-1.0 — aarch64 ISO uses U-Boot path]`
- `ACTION_CAPTURE_FRAME` — `[DEFERRED: post-L7, no hardware path]`
- `ACTION_SCAN_ENVIRONMENT` — `[DEFERRED: post-L7, needs SLAM3R]`
- `ACTION_QUERY_SCENE` — `[DEFERRED: post-L7, no scene graph]`
- `ACTION_PARSE_DOCUMENT` — `[DEFERRED: needs 64 MiB OCR model, no model-shipping path]`
- `ACTION_EXEC_CODE` — `[DEFERRED: needs sandboxed interpreter subsystem]`
- `ACTION_BROWSE_WEB` — `[DEFERRED: deferred post-1.0 (DDR-793) — cloud bridge is a security-posture change]`
- `arch/aarch64` full port — `[DEFERRED: boot-only scope per ADR-034 — ISO uses boot-only kernel]`
- `arch/riscv64` full port — `[DEFERRED: boot-only scope per ADR-034 — ISO uses boot-only kernel]`
- Cloud bridge activation — `[DEFERRED: deferred post-1.0 (DDR-793)]`
- Rust rewrite — `[DEFERRED: not in scope]`
- `CAP_OCR` / `CAP_SCENE` with no hardware path — `[DEFERRED: capability bit defined, enforcement deferred — no subsystem path]`
- SFS block reclamation on-disk — `[DEFERRED: in-memory reclaim shipped (DDR-762-v2); on-disk free-tree deferred post-1.0]`
- NVMe completion IRQ — `[DEFERRED: poll-mode sufficient for ISO; DDR-774a/b/c deferred until B#3 SMP stable]`
- `ACTION_SEND_IPC` — `[DEFERRED: no ring-3 IPC surface — ipc_send/ipc_recv are kernel-internal and capability-gated and there is no SYS_IPC_*, so an approved SEND_IPC has no executor in any ring; building it is new kernel ABI plus a security decision (DDR-1017 §1)]`
- `ACTION_RUN_EXPERIMENT` — `[DEFERRED: no implementation at any ring — CAP_EXEC is a #define checked nowhere (zero matches in kernel/*.c, no is_exec on struct tcb), there is no experiment subsystem, and the metric lockbox is CAP_SOVEREIGN read-only by design so an agent cannot record a result; it is ACTION_EXEC_CODE's deferral under another name (DDR-1021)]`
- F#74 capability discovery — `[DEFERRED: agent_caps exists on struct tcb (DDR-982) but is initialised to 0 and never granted, and there is no syscall to read it; building it reverses DDR-982's deliberately withdrawn per-slot enforcement (DDR-1022 §4)]`
- F#66 architect_agent / F#67 healer_agent (RUFLO) / F#69 inventor_agent / F#70 tournament_agent / F#71 subconscious world model / F#72 verifier_agent / F#75 lineage memory — `[DEFERRED: domain behaviour with no subsystem behind it; there is exactly ONE agent program (user/agent_base.c) and the named agents are roster slots, so each of these is a behaviour rather than plumbing. A stub would gate vacuously, and F#75's gate would duplicate smoke-agentmem (DDR-1022 §2/§5)]`
- F#73 sovereign NL UI — `[DEFERRED: needs a natural-language surface; blocked on the same two missing pieces as Ctrl+Alt+T — no windowed terminal client, and sys_exec.c:47 discards argv/envp so a spawned client cannot be told what to attach to (DDR-1022 §5)]`

### This resolves DDR-982 §5.5 without a new operator ruling

DDR-982 §5.5 asked whether the four absent action types should be declared just
to have a capability boundary to enforce, and said *"this reverses a documented
repo decision either way, so it is the operator's call, not mine."* It is
already the operator's call, made in advance, in two rows of this same table:

- `CAP_OCR` / `CAP_SCENE` → **"capability bit defined, enforcement deferred — no
  subsystem path"**. DDR-982 §2 records that bits (A) and the `agent_caps` field
  (B) are already built and that enforcement (C) plus its gate (D) were
  withdrawn. **That is exactly the pre-approved state.**
- each of `ACTION_PARSE_DOCUMENT`, `ACTION_QUERY_SCENE`, `ACTION_EXEC_CODE` and
  `ACTION_BROWSE_WEB` is separately listed as deferred with its own reason — so
  "leave them absent", which DDR-982 §5.5 leaned to, is the standing decision.

**Consequence for the Group F rows.** PRAX, LUMYN, AHNIS and IRIS stay unbuilt,
but they are **not blocked on a ruling** — they are blocked on subsystems that
are themselves pre-approved deferrals (a sandboxed interpreter, a cloud bridge,
a 64 MiB OCR model, a scene graph). Declaring the bits would not make any of
them work; it would add three action types that can only return "not
implemented". The queue rows are corrected to say that.

---

## DDR-1013 — a probe constant drifted from the wire format; and what 3C work really is

**FIX.** `user/actiondagtest.c` defined `ACTION_PRINT 1`. `aether.h` pins the
wire format with `_Static_assert`s — `ACTION_WRITE_FILE == 1`,
`ACTION_PRINT == 2` — so every action `smoke-actiondag` submitted was queued and
**audited as a file write** while every line of the probe called it a print.

**No gate caught it, and the reason is the useful part.** `smoke-actiondag`
asserts two sentinels, and the DAG logic it tests is type-agnostic. The one
type-sensitive predicate in the submit path is
`aether_action_forces_pending()` — and `WRITE_FILE` and `PRINT` are **both
outside** that set, so they behave identically there. The bug was invisible by
coincidence, not by being harmless: add `WRITE_FILE` to that set, or repoint the
probe, and the gate changes meaning silently.

**SCOPE CORRECTION, recorded because I nearly acted on the wrong reading.** The
eight declared Section-3C action types are referenced nowhere but the enum and
that predicate, which looks like the kernel approving actions nothing
implements. It is not. `agent_base.c` states the architecture: the agent
*proposes*, the kernel rules, **the agent executes**. There is no kernel-side
executor for `ACTION_WRITE_FILE` either. So a declared type with a policy entry
and no ring-3 implementer is an unused vocabulary entry, and the submit-time
rejection I was about to add would have broken the force-pending policy that
already names three of those eight as human-gated.

DDR-1013 §2.1 specifies what implementing one actually requires, and notes that
the eight fall into two policy classes whose gates must differ.

### DDR-1010 §9 — the campaign result: 36/36 clean, and why that is not a closure

Kernel `9623c163cd479043`, one hash on **both sides of every run**: **36 runs,
36 PASS, 36/36 serial captures kept, zero `gs FAIL`, zero `apfreeze`.**

`smoke-blk-integrity` runs through `boot_test.sh`, so all three strings are
`GLOBAL_FORBIDDEN` entries and PASS implies their absence. The detector is shown
live two ways: every capture carries `[percpu] gs OK (syscall ctx)` from the
one-shot probe, and the continuous probe is proven by the `if (0)` mutant
(`6c81563d46114d5c`), not by its silence — it prints only on failure by design.

**What this establishes:** the local per-run rate on this kernel is below ~8% at
95% confidence (`0.92³⁶ ≈ 0.049` — the power figure derived *before* the run).

**What it does not:** that the defect is gone. It reproduced on the **first** run
of an earlier session; one failure in four cannot separate p=0.25 from p=0.05.
Nothing since has touched `swapgs`, percpu, or the syscall entry discipline. And
the probe itself adds a `this_cpu()` read plus a compare to **every syscall** —
work added to exactly the timing-sensitive path where the race lives, so it may
perturb what it measures.

**Next experiment, if one runs (§9.3):** not another 36 here — campaign the
**pre-probe** kernel `29c792a8b8f3b056`, the one the failure was observed on.
Either answer is informative and neither is assumed. The source defect remains
**not named**.

---

## DDR-1014 — `sched_unblock` spent its one kick on a CPU that never receives one

**FIXED, two defects, one found by reading and one by a CI artefact.**

**The kernel defect.** `smp_resched_one` declines for the BSP and said nothing
about it; `sched_unblock` `break`s out of its search **on the call**, not on a
delivery. So an unblock running on an AP that finds the BSP idle first sends
nothing and stops looking — any idle AP later in the list waits a full timer
tick, which is exactly the latency rq-3 exists to remove. Reachable in ordinary
operation: `virtio_blk.c:102/198` call `sched_unblock` from MSI-X interrupt
context on whichever CPU the vector is routed to. A latency defect, not a
correctness one — the timer backstop holds, so nothing hangs and nothing
announced it. Fixed by having `smp_resched_one` report a *delivered* IPI and
breaking only on that.

**The probe defect, and the artefact.** CI on `72a474a` (one Markdown file over
`483e853`, so the identical kernel) reddened **`smoke-percpu-sched`** with
`[smp] resched FAIL ipis=0 ran=1 idle=1` — a gate that does not own that
assertion, caught because `resched FAIL` is a `GLOBAL_FORBIDDEN` entry (DDR-791
working). DDR-1004's proof scanned for an idle non-self CPU **without** the
kernel's fourth clause, `!is_bsp`, so whenever the BSP was the idle one it
expected an IPI the kernel would never send. Deterministic, no timing needed.

**This corrects DDR-1004 §6.1**, which predicted a residual and named the wrong
mechanism — a timing race (real, but narrow) rather than a predicate mismatch.
The `idle=` field DDR-1004 added is what made the CI line readable, so the
instrument worked; what it read out was not what §6.1 expected.

**Non-vacuity, the load-bearing check:** narrowing `idle_seen` could have made it
always false and retired the assertion into permanent `SKIP`. Three consecutive
`[smp] resched OK` on `smoke-rqstress` with the capture pinned say otherwise.

Kernel `c9740c9a61332f37`, `-Werror` clean. `smoke-percpu-sched`, `smoke-rqstress`,
`smoke-resched`, `smoke-smppreempt`, `smoke-crosswake`, `smoke-smpsched`,
`smoke-blk-integrity`, `smoke-blkmq`, `smoke-shell` all PASS; `ci-shard-check` OK
(158/10/7); `ci-probe-rodata-check` OK.

**Not measured:** the latency the fix recovers. No gate times a wake, so it can
only be shown not to break anything.

---

## DDR-1015 — Section 3C `ACTION_READ_FILE`, end to end (1 of 8)

**DONE.** `smoke-actionread` (shard 1, fast), M1 mutation-checked. Kernel
`b0e4ccb83d4bb7ac`, 1,106,314 B, `-Werror` clean.

Built to DDR-1013 §2.1's shape, which corrected the wrong reading of this work:
a 3C type is **ring-3** work, because the kernel is the policy engine and the
agent is the executor — there is no kernel-side executor for `ACTION_WRITE_FILE`
either. The probe proposes, waits for the verdict, and only then opens and reads.

**The gate asserts the effect:** `PRADYOS_ACTIONREAD_OK id=258 n=25 first=P` —
25 bytes is exactly `len("PRADYOS filesystem works!")`, the content of
`/HELLO.TXT`. **M1** (read skipped, kernel `6edc2e3889fd847b`) keeps `first='P'`
deliberately so only the byte-count arm can catch it, and it does: `n=0` → FAIL.
The sentinel and the first-byte check both passed on a probe that never read.

**`ACTION_READ_FILE == 5` is now pinned** by `_Static_assert` beside the three
existing wire-format pins, because DDR-1013 §1 found `actiondagtest.c` had
drifted to a wrong constant with no gate able to see it. The kernel now stops
building if the enum shifts.

**NOT proven: the ordering.** The gate shows the pipeline ran and the read
happened, not that the read happened *after* approval — and the ordering is the
authority property. Recorded unmeasured with its reason. §5 names what would
measure it: submit an action the policy **rejects** and assert the read does not
happen, the one case where the two orders differ observably. That is also the
natural shape for the `DELETE_FILE` gate, which needs a PENDING assertion anyway.

Seven types remain; three of those are force-pending.

---

## DDR-1016 — Section 3C `ACTION_DELETE_FILE`, and the ordering (2 of 8)

**DONE.** `smoke-actiondel` (shard 1, fast), M1/M2 mutation-checked on distinct
kernel hashes. Kernel `bf6f7c80ed07040f`, 1,114,506 B, `-Werror` clean.

The **first force-pending** type, so the gate asserts the OPPOSITE of
`smoke-actionread`: the verdict must stay `AE_PENDING` and the file must
**survive**. `PRADYOS_ACTIONDEL_OK id=258 st=1 ctrl=1 keep=1`.

**This closes DDR-1015's unmeasured ordering**, exactly where §5 predicted: a
read leaves no trace so both orders print the same line, but a delete does, so
act-then-ask is visible in the filesystem. `keep=1` is that measurement.

**`ctrl=1` is the control.** The probe deletes a second file outright, no action
involved, through the same `SYS_UNLINK` — without it a broken unlink would make
`keep=1` true for the wrong reason and M1 would pass.

**A force-pending probe cannot busy-poll.** Measured: the first draft copied
DDR-1015's loop and the kernel killed the agent (`AGENT_RATE_LIMITED PID=37`,
new in that capture, absent from the baseline). `AETHER_RATE_MAX` is 60
syscalls/second; DDR-1015's loop is safe only because an auto-approved action
breaks it on iteration 1. The fix is a **ring-3 spin** between two polls — zero
syscalls, still preemptible. The other three force-pending types will hit this.

**The `st` arm was dead until M2 found it.** `aether_poll` frees the slot on any
terminal verdict, so an unconditional second poll returned `-ESRCH` and the
printed `st` could only ever be 1. Now the second poll happens only if the first
says PENDING; M2 reports `st=2` and the gate names the defect.

| mutant | kernel | result | arm |
|---|---|---|---|
| M1 probe acts before the verdict | `4075ae6e2d6015b1` | `keep=0` FAIL | `keep` only |
| M2 kernel drops DELETE_FILE from `forces_pending()` | `7c86311198e18e7a` | `st=2` FAIL | `st` only |

**Also fixed here:** `user/actionreadtest.c` (DDR-1015) shipped a `_start`
without `force_align_arg_pointer`. `ci-start-align-check` catches it and runs in
CI, but CLAUDE.md's hygiene list names only two of the three static checks — so
run `tools/ci/hygiene_check.sh`, not the list. Six types remain.

---

## DDR-1017 — Section 3C `ACTION_SPAWN_PROCESS` (3 of 8), and a blocked type

**DONE.** `smoke-actionspawn` (shard 2, fast), M1/M2/M3 on distinct kernel
hashes. Kernel `30658af9358ab055`, 1,118,602 B, `-Werror` clean.
`PRADYOS_ACTIONSPAWN_OK id=258 st=1 ctrl=1 post=-10`.

**`ACTION_SEND_IPC` is BLOCKED, not skipped.** `ipc_send`/`ipc_recv` are
kernel-internal and capability-gated (they take a `struct cap_table *`), and
there is **no `SYS_IPC_*`** — so no agent can execute an approved SEND_IPC. It is
in the enum, so it can be submitted and approved today with no way to act on it.
Building it needs a new NSI (97 free) plus a capability check and a nameable
endpoint: new kernel ABI and a security-surface decision, not a probe.
`QUERY_MEMORY` needs the same check before it is budgeted as probe work.

**The effect is asked of the kernel**, since a filesystem cannot witness a
process: `wait4(-1, &st, WNOHANG)` → `-10` (`-ECHILD`) required; `-11` or any
positive pid means a fork happened on a PENDING action.

**`ctrl` had to be made computed.** The first draft `fail()`d on control
mismatch and printed a literal `ctrl=1`, so the gate's ctrl check could never
fire — the same dead arm DDR-1016 §5 found. **M3 exists to prove it is live.**
Class worth naming: *a field whose only reachable value is the passing one is
decoration, not measurement.*

| mutant | kernel | result | arm |
|---|---|---|---|
| M1 probe forks on PENDING | `5cd2db8a5d2a68ca` | `post=45` | `post` only |
| M2 kernel drops SPAWN_PROCESS from `forces_pending()` | `a09869767ad0ef1a` | `st=2` | `st` only |
| M3 control child wrong exit status | `1ea29f035d1b296f` | `ctrl=0` | `ctrl` only |

M1/M2 mutate the system; M3 mutates the gate's own control. M1 was re-run against
the shipped probe after the ctrl refactor — a mutation result on code that was
not shipped is not a result.

**Also fixed:** `${ln##*st=}` read `st` out of `post=` (both end in `st=`), which
is how the gate first failed on a correct measurement. Both this gate and
DDR-1016's now anchor every field on its leading space; DDR-1016's parsed
correctly only by luck of field naming. Five types remain, two of them blocked.

---

## DDR-1018 — Section 3C `ACTION_QUERY_MEMORY` (4 of 8), correcting DDR-1017 §1

**DONE.** `smoke-actionquery` (shard 6, fast), M1/M2 on distinct kernel hashes.
Kernel `c928493492bba59e`, 1,126,794 B, `-Werror` clean.
`PRADYOS_ACTIONQUERY_OK id=258 st=2 n=24 first=Q`.

**CORRECTION.** DDR-1017 §1 flagged `QUERY_MEMORY` as possibly blocked like
`SEND_IPC`. It is **not**: `SYS_MEMORY_WRITE` (82) / `SYS_MEMORY_READ` (83),
CAP_MEMORY, shipped in DDR-836 and already exercised by `user/agentmemtest.c`.
So it follows DDR-1015's auto-approving shape. **`SEND_IPC` stays blocked**,
re-checked by an exhaustive grep of every `#define SYS_` for
`ipc|chan|msg|endpoint|bcast|send|recv|port` — only socket-connect, two
surface-send calls and net-allow, none an agent-to-agent channel.
Section 3C is **4 of 8, 1 blocked, 3 to go.**

**The dead-arm class, a third time, caught before the mutants ran.** The draft
`fail()`d on any non-APPROVED verdict, so the printed `st` could only ever be 2.
Now reported, not asserted. The poll was also bounded to DDR-1016's two-poll
shape — and M2 proves that mattered: under a permanently-PENDING verdict the
20000-iteration loop would have hit `AETHER_RATE_MAX` and had the agent killed
before printing anything.

| mutant | kernel | result | arm |
|---|---|---|---|
| M1 claim success without reading (`first='Q'` kept) | `85d57430833c879d` | `n=0` | `n` only |
| M2 kernel forces QUERY_MEMORY pending | `baccd11d421d0c5c` | `st=1 n=0 first=?` | `st` + `n` |

M2 moving two arms is **correct, not sloppy**: for an auto-approving type a
verdict that never arrives means the read must not happen, so they are coupled by
design. `first=` is recorded as **not independently mutation-checked**, with the
reason.

**Also fixed:** `aether.h` carried the DDR-1016 `DELETE_FILE == 6` pin twice,
shipped in `5d2efd5` — legal C11, so nothing failed and no gate could see it.
From restoring the header after a mutation and re-applying the edit on top;
`git diff` was the check that would have caught it. Three types remain.

---

## DDR-1019 — the shard-9 `[apfreeze]` is a PANIC SYMPTOM

**MECHANISM NAMED + instrument built and mutation-proven. No fix.**
Kernel `6836dc723f31fc3e`, 1,126,794 B, `-Werror` clean.

CI run 33323162053, shard 9, `smoke-blkmq-trace`, `6894062`.
`[apfreeze] cpu=3 rip=0xFFFFFFFF8000A2F8 if=0` — and that RIP, resolved against
the CI binary **rebuilt bit-for-bit** (`b0e4ccb83d4bb7ac`), is the
`for(;;) cli; hlt` at `idt.c:697`: the LOSING branch of DDR-979's one-winner
panic latch. **That CPU panicked.** The frozen ticks, `if=0`, the two
`[vblk] compl wait timeout dest_cpu=3` and the gate failure are all downstream.

**The winner printed nothing.** `g_panic_extra` increments only on losing the
CAS, so a winner existed — but `NEXUS KERNEL PANIC` is in `GLOBAL_FORBIDDEN` and
`smoke-blkmq-trace` goes through `boot_test.sh`, so a banner would have killed
the run there; it ran ~1000 more ticks instead. The latch is claimed **before**
the dump and never released, so a winner that cannot print silences every later
panic and leaves only frozen CPUs. DDR-979 traded a garbled dump for a readable
one; the failure mode it introduced is **no dump**.

**Not DDR-1010's SWAPGS path** — that probe is in this kernel and printed zero
`gs FAIL` lines, i.e. the instrument excluded itself.

**Instrument:** `g_panic_stage` (how far the winner got) plus the first loser's
`cpu`/`vec`/`rip`, **recorded not printed** — a loser that printed would
reintroduce the interleaving DDR-979 removed — surfaced only inside the existing
`if (g_panic_extra)` heartbeat block, so a healthy boot emits nothing extra.

**M-B proves it and reproduces the CI shape locally.** An AP forced down the
loser branch (`ud2` from `sched_tick`, latch pre-claimed), kernel
`640fdd2c17451143` at `-smp 4`:
`panics_silent=1 panic_stage=0 loser_cpu=3 loser_vec=6 loser_rip=0xFFFFFFFF800122D4`,
`[apfreeze] cpu=3 … if=0`, zero banners. `loser_vec=6` is `#UD` as injected and
`loser_rip` is `sched_tick + 0x74`, the injection site.

Two failed injection attempts are recorded as reusable: injecting on the **BSP**
halts the machine before any heartbeat, and `*(volatile uint64_t *)0x8 = 0` does
**not** fault — page 0 is mapped writable.

**`[apfreeze]` has at least three producers.** DDR-1006's RIP backtraces through
`sched_tick`, DDR-1010's through `sys_mmap`, this one is the halt loop. Resolve
the RIP against its own binary before matching on the sentinel name.
A latch-liveness watchdog is named in §8 and deliberately NOT built.

---

## DDR-1020 — `PROPOSE_HYPOTHESIS` + `EVOLVE_GENOME`; 3C is 6 of 8

**DONE.** `smoke-actionhypo` (shard 3, fast). Kernel `53fe179c85a7c3b5`,
1,134,986 B, `-Werror` clean.
`PRADYOS_ACTIONHYPO_OK hst=2 hn=33 gst=1 gseed=9 gn=9`.

**One probe, one boot, both sides of the policy split.** `PROPOSE_HYPOTHESIS` is
auto-approving, `EVOLVE_GENOME` is force-pending, so `hst=2` beside `gst=1` shows
`aether_action_forces_pending()` actually discriminates — a comparison two
separate gates could not make.

**Two accounting corrections.** `ACTION_SPAWN_PROCESS` is **not** one of the
eight (pinned as pre-existing), and `ACTION_REWRITE_AGENT_CODE` was **already
shipped and gated by DDR-842** (`smoke-coderewrite`, shard 7, strict — four
capability roles and a real approver via NSI 86). DDR-1017 §7 and DDR-1018 §7
were wrong on both. **True tally: 6 of 8 shipped, `SEND_IPC` blocked,
`RUN_EXPERIMENT` remaining.**

| mutant | kernel | result | arm |
|---|---|---|---|
| M1 skip the approved log | `35157cf009afe975` | `hn=0` | `hn` |
| M2 force HYPOTHESIS pending | `977cbdef336a287f` | `hst=1 hn=0` | `hst` |
| M3 drop GENOME from `forces_pending()` | `ee069299dd4fec34` | `gst=2` | `gst` only |
| M4 evolve on a PENDING verdict | `26236c25931f9fe2` | `gn=17` | `gn` only |
| M5 genome seed never lands | `f56434e9e879f639` | `gseed=0 gn=0` | `gseed` |

**M4 caught a gate arm I had convinced myself was sound.** Its first version
rewrote `/GENOME.TXT` in place and the gate **passed** — because the write
returned short (`put_rc=-1`), not because policy stopped it. A same-length
rewrite failed too; only unlink-then-recreate succeeded. **Unexplained SFS
behaviour, recorded and NOT fixed** — the ADR-032 write budget is excluded,
since the unlink+create at the same point in the same boot succeeded. Lesson:
*a mutant that fails to perform the defect is indistinguishable from a gate that
catches it.*

**Dead-arm class, instances four and five**, now stated as a rule: **a probe
should REPORT and let the gate JUDGE**; every `fail()` before the print silently
removes an arm.

---

## DDR-1021 — `ACTION_RUN_EXPERIMENT` assessed: not buildable. Section 3C CLOSED.

**ASSESSMENT, no code change.** Grepped before budgeting it, per DDR-1020 §6.
All three things it needs are absent: `CAP_EXEC` is a `#define` in `cap.h:62`
**checked nowhere** (zero matches in `kernel/*.c`, and no `is_exec` on
`struct tcb` unlike the `is_memory`/`is_rewrite` fields that make CAP_MEMORY and
CAP_REWRITE real); there is **no experiment subsystem** (`grep -rln experiment`
returns nothing); and the metric lockbox is **`CAP_SOVEREIGN` read-only** by
design, so the agent being measured cannot write its own result. It is
`ACTION_EXEC_CODE`'s existing deferral under another name.

Distinct from `SEND_IPC`, which *has* a working kernel-internal implementation
and lacks only a ring-3 door. `RUN_EXPERIMENT` has none at any ring.

Both are now logged in the deferrals list above, so §WHAT "DONE" MEANS's *"zero
unlogged exclusions"* holds for Section 3C.

**Section 3C final: 6 shipped and gated, 2 deferred with reasons, 0
buildable-and-unbuilt.**

---

## DDR-1022 — Group F assessed; TWO items were already shipped and gated

**ASSESSMENT, no code change.** Grepped before budgeting, per DDR-1021's method —
and the grep contradicted this tracker **twice**:

- **F#68 metric lockbox e2e** — this file said *"kernel ✅ Python ✅, e2e wiring
  unverified"*. It is **shipped and gated**: `user/lockboxtest.c` reads via
  `SYS_METRIC_READ` (NSI 76), verification happens before any bytes are copied
  (`-ETAMPER` yields nothing), gate **`smoke-lockbox`** (shard 7, strict),
  DDR-812. `smoke-lockbox-e2e` does not exist and is not needed.
- **F#76 tamper-evident ledger** — this file said *"⬜ not started"*. It is
  **shipped and gated twice**: `SYS_READ_AUDIT` (37) + `SYS_VERIFY_AUDIT` (93),
  with `smoke-auditchain` (shard 0, strict) AND `smoke-auditchain-tamper`
  (shard 4, strict). Gated intact *and* tampered is what "tamper-evident" asks.

**The structural fact that reframes "11 unbuilt agents":** there is exactly ONE
agent program, `user/agent_base.c`. The roster is generic active-bits and a slot
is filled by `SYS_SPAWN_AGENT` launching that template with a task; the kernel
holds no per-agent identity. So the remaining items are **domain behaviours, not
programs** — and a stub agent produces a vacuous gate, the trap this session hit
five times.

F#74 is blocked by a decision already taken (DDR-982's withdrawn per-slot
enforcement). The other eight are deferred above with reasons.
**Nothing in Group F is now both buildable and unlogged.** It does NOT claim
Group F is feature-complete — eight agents and an NL UI are genuinely absent.

---

## DDR-1023 — pre-probe campaign: 0/20, and the local route is exhausted

**CAMPAIGN RUN, null result, no code change.** Kernel `29c792a8b8f3b056`
(rebuilt bit-for-bit from `d7d2794`, the commit before the probe landed),
`smoke-blk-integrity`, **N=20, thresholds fixed before starting**.

**20/20 pass, zero failures, one hash across all 40 recorded values, zero drift.**

**DDR-1010 §9.2's perturbation hypothesis is NOT supported.** The probe is not
what made that 36/36 campaign clean — the kernel without it is clean too. The two
campaigns bound their own binaries and **must not be pooled** (DDR-1009 §8.3):
36/36 → rate < 8%; 20/20 → rate < 14%; both 95%. At the originally-observed 25%,
`P(0 in 20) = 0.0032`.

**OPEN-2 is not closed** — what changed is where the evidence lives. The local
reproduction route is **exhausted** (56 clean runs across both kernels, including
the exact binary the failure was first seen on); the live evidence is CI-side,
where DDR-1019 showed one `[apfreeze]` was a panic symptom and armed the
instrument for the next. **The "~1 in 4" figure should not be quoted again.**

**Methodology defect recorded (mine).** The runner's captures were **make
output, not serial logs** — 3010 B each, zero `[hb]` lines — because the glob for
a capture file matches nothing on a passing run. A grep for `apfreeze` over them
was therefore vacuous, and I nearly reported it as evidence. The §3 claim stands
only on rc=0 plus `GLOBAL_FORBIDDEN` (all three sentinels are in that list and
this gate uses `boot_test.sh`). Same class as the five in-gate instances this
session — it recurs in **campaign tooling** too. Next campaign: point
`SERIAL_LOG` at a per-run path and assert the file contains boot output before
scanning it.

---

## DDR-1024 — OPEN-13: DDR-986's instrument, built and proven

**IMPLEMENTED + mutation-proven. NOT a fix.** Kernel `0e9dfefadf54d6ba`,
1,134,986 B (unchanged — the store lands in existing free-object space),
`-Werror` clean.

DDR-986 designed this in full and it was **never built** — zero
`__builtin_return_address` in `kheap.c`. OPEN-13 has one capture, in CI, never
locally, so an instrument is the only thing that makes the next one readable.

**The site is captured at the PUBLIC boundary** (`kfree`, `pcb_free`,
`cap_free`, `ipc_free`) and threaded through `kfree_locked`/`pool_free` into
`cache_free(c, ptr, site)`. That is DDR-986 §4/§5's correction and it is
load-bearing: `cache_free` is `static` behind two wrappers, so a builtin inside
it would name a wrapper and make `freed_by`/`now_by` two different stack frames.

Stored at **offset 16** of a free object (`next@0`, `canary@8`), after the
`memset`, in a line the `memset` already touched. Guarded by `obj_size >= 24`,
so the 16-byte class and the dedicated `cap` cache keep today's output. **On
whenever `KHEAP_DEBUG` is** — opt-in would be OFF in CI, the only place OPEN-13
has ever appeared.

**M1** (deliberate 128-class double free, kernel `18ecdfe77265e799`):
`objsize=0x80 freed_by=0xFFFFFFFF800052DC now_by=0xFFFFFFFF800052F4`, resolving
against that exact binary to `fs_test_thread + 0x2FBC` and `+ 0x2FD4` — the two
injected call sites, `0x18` apart.

Gates on the baseline, one hash verified before and after each: `smoke-shell`,
`smoke-blkmq`, `smoke-fsrm`, `smoke-blk-integrity` — all PASS (the last two
chosen for being the heaviest `kfree` traffic in the suite).

**OPEN-13 is NOT closed.** One capture, no mechanism named. The change is that
the next occurrence names two call sites instead of a generic size class.

---

## DDR-1025 — `smoke-mouse` has been passing on a 1-in-5 margin

**MEASURED, no fix.** Kernel `2605f6d2b571e746`, 1,134,986 B, `-Werror` clean.

`smoke-mouse` failed on shard 2 on two documentation-only commits, and a
`workflow_dispatch` re-run of the identical commit passed — **2 failures in 6
suite-runs** on kernel `53fe179c85a7c3b5`. Two counters were added instead of a
retry, as PR #17 comment 5471708345 committed to.

**On a PASSING run:** `btnedge=5 mpoll=205573 mbtn=1 btn1drain=0`. Five press
edges reached the driver; across ~205,000 ring-3 polls **exactly one** returned a
button down. **The pointer path drops four of five clicks even when the gate is
green** — so a run where zero get through needs no new defect, and this gate has
never had the margin its green implied.

**`btn1drain=0` refuted my own hypothesis:** press and release are never
coalesced into one virtqueue drain, so DDR-941's "invisible BY CONSTRUCTION"
case is not what happens here. `virtio_input_state()` does not consume on read
either. A local experiment shortening the click hold 200 ms → 4 ms still passed.

**Why one press in five is visible is NOT established** (~1,800 polls/s over a
200 ms hold should span ~350 polls), so §NON-NEGOTIABLE 3 forbids a fix.
**[ANSWERED — DDR-1026. The reading in this paragraph is wrong: `mpoll` is
cumulative over the whole boot, and ring 3 had not polled ONCE when the clicks
landed. See the DDR-1026 section below.]** The
repair would be an **edge latch** in `SYS_MOUSE_POLL` — a genuine product
improvement, since a desktop dropping 80% of clicks is user-visible — but it is a
kernel ABI semantic change resting on an unexplained mechanism, and a
timeout/retry bump is explicitly not the alternative.

`smoke-mouse`, `smoke-input`, `smoke-compositor` all PASS; `hygiene_check.sh`
all three PASSED.
 `READ_FILE` (1015) · `DELETE_FILE` (1016) ·
`QUERY_MEMORY` (1018) · `REWRITE_AGENT_CODE` (842) · `PROPOSE_HYPOTHESIS` +
`EVOLVE_GENOME` (1020); `SEND_IPC` and `RUN_EXPERIMENT` deferred.

**Residual recorded, not fixed:** both deferred types are IN `enum aether_action`,
so an agent can submit either and `SEND_IPC` will auto-approve in sovereign mode
with nothing to act on it — which contradicts `aether.h`'s own stated policy for
the six types it deliberately omits. Not changed: the enum is append-only wire
format (DDR-832) and the queue entry is bounded and audited (it expires via
`AETHER_ACTION_TTL_TICKS`), so the cost is a wasted slot, not an unguarded
action. Post-1.0 decision.


## DDR-1026 — the `SYS_MOUSE_POLL` press-edge latch

**IMPLEMENTED + GATED + mutation-checked.** Kernel `56a4c4a35c92cfc5`,
1,134,986 B, `-Werror` clean.

**This corrects DDR-1025 §5 on its central reading.** `mpollwin=0` was never an
anti-correlation between ring 3's poll cadence and the injector. `mpoll` is
**cumulative over the whole boot**, and the failure is at the start of it:

```
btnedge=0 mpoll=0 mbtn=0     <- ... eleven heartbeats like this ...
btnedge=3 mpoll=0 mbtn=0     <- THREE presses landed. Ring 3 has polled 0 times.
btnedge=5 mpoll=1 mbtn=1     <- the first poll of the entire boot, after all five
btnedge=5 mpoll=3481 mbtn=1
```

Ring 3 was not missing each 200 ms window — **it had not polled once.** All five
clicks are injected before the compositor's input loop takes its first sample,
so a counter summed over 80 s said nothing about the six seconds that mattered.
`smoke-mouse` fires on `PRADYOS_AMBIANCE_OK`, which means the ambiance render
finished, not that input is being serviced; `mouse_inject.sh` has carried
DDR-910's outcome-driven retry for exactly this situation, and this gate alone
never adopted it.

**The fix is still the latch, not the retry argument.** Retrying would green the
gate by clicking until one landed, while a real user's clicks in that window
stayed gone. `SYS_MOUSE_POLL` exposes current state, not an event queue
(DDR-941), and a state poll cannot represent an edge that has already ended —
the driver saw all five presses, and ring 3 asked correctly, just late.

One word of state (`g_btn_latch`) set on the same edge that increments
`g_btn_edges`, drained read-and-clear at the syscall. `virtio_input_state()`
stays **pure**; `virtio_input_wheel()` (DDR-725) has used read-and-clear since
Layer 7 for the same reason.

| build | kernel | result |
|---|---|---|
| fixed | `56a4c4a35c92cfc5` | **PASS 4/4**, identical `mbtn=1 mouse_ok=1` |
| M1 (drain, do not deliver) | `698ac2d1ceaad30d` | **FAIL**, `mbtn=0 mouse_ok=0` |

4/4 identical is the result worth stating: the gate used to fail ~2 runs in 6
with `mbtn` reading 0 or 1 unpredictably. A latch cannot be lost to timing.

**Sixth dead-arm instance, and measured rather than reasoned.** `mouse_ok >= 1`
implies `mbtn >= 1`, so with the ring-3 arm first the new kernel-side arm could
never be the thing that fires — M1's first run tripped `PRADYOS_MOUSE_OK` and
never reached it. The kernel arm now runs **first**, and the pair splits the
failure: `mbtn=0` means the syscall never delivered; `mbtn>=1` with no
`PRADYOS_MOUSE_OK` means it delivered and ring 3 did not act.

**Residual, recorded not fixed:** the latch is a bitmask, not a counter, so
repeated clicks between two polls still coalesce into one, and a missed
**release** is still missed — that needs an event queue, a far larger ABI
change. The latched press is also reported at the current pointer position, not
the position at the press edge.

`smoke-mouse` 4/4, `smoke-drag`, `smoke-agent-click`, `smoke-resizeall`,
`smoke-shell` 5/5, `smoke-blkmq`, `smoke-rqstress-liveness`,
`smoke-blk-integrity` all PASS; `hygiene_check.sh` all three PASSED.


## DDR-1027 — Ctrl+Alt+T launches a PRISM terminal window

**IMPLEMENTED + GATED + mutation-checked (M1/M2/M3).** The last unbuilt Group E
row. Kernel `0d1bcd234707e56d`, `term.elf` `55ad497f47b6d64a`, `-Werror` clean.

**The row understated the work.** PRISM reads fd 0 and writes fd 1
(`prism.c:96`, `:175`), which today are the serial console, and every existing
surface client draws coloured rectangles. There was no terminal *window*
anywhere in the tree to launch. Three pieces, all on shipped primitives:

- the chord, off the DDR-991 event ring (`SYS_KEY_POLL`), because the byte
  stream carries no modifier state and DDR-992 already stops a non-Shift chord
  emitting text — so nothing on this system loses the letter `t`;
- `user/term.c`, a client that owns a surface, forks PRISM over a pipe pair,
  renders with the Inter atlas and forwards the keys the compositor routes to
  the focused window;
- `/TERM.ELF` and `/PRISM.ELF` placed on the FAT volume, because `execve`
  resolves against the **process** root — the same volume `/EXECTEST.ELF` sits
  on and systest's `execve` is already proven against.

**The one missing primitive.** There is no `O_NONBLOCK` and no `fcntl` in this
kernel. A plain `read()` on PRISM's stdout pipe would block whenever PRISM had
nothing to say, and the window would stop draining its own key ring — it would
accept input only just after output. `SYS_EPOLL_WAIT` with **timeout 0** is the
replacement, and it is the design's only non-obvious shape.

**`fork`+`execve`, not `SYS_SPAWN_AGENT`.** That is the AETHER roster path: it
would consume a fixed roster slot, mint agent capabilities, and list a terminal
in `SYS_AGENT_ROSTER` as an autonomous agent. A terminal is an application.

### Arm E, and the mutation plan that did not survive

The design proposed testing the chord by injecting a bare `alt-t` and asserting
no second spawn. **Unrunnable:** `input_inject.sh` replays its whole key list
four times and the compositor caps terminals at four, so a chord-less build and
the correct one both report four spawns. Counting proves nothing.

The compositor now reports every `'t'` press with its modifier byte:

```
PRADYOS_TERM_CHORD mods=6 spawn=1      <- Ctrl(2)|Alt(4): spawned
PRADYOS_TERM_CHORD mods=4 spawn=0      <- Alt only: did not
```

and arm E fails on any `spawn=1` whose `mods` lacks `KMOD_CTRL`. That is a
permanent arm re-checked every CI run, not a one-off. Clean build measured:
**8 t-presses, 4 spawns**, strictly alternating.

| mutant | change | binding hash | outcome |
|---|---|---|---|
| — | clean | kernel `0d1bcd234707e56d` / `term.elf` `55ad497f47b6d64a` | **PASS** |
| M1 | compositor tests `KMOD_ALT` only | kernel `c2462fb0de8231c9` | **FAIL at arm E** |
| M2 | child skips `dup2(from_sh[1], 1)` | `term.elf` `67158f82326cf9ae` | **FAIL at arm C** |
| M3 | terminal skips `SURFACE_RAISE` | `term.elf` `56028a4dbfe993f8` | **FAIL at arm D** |

**Hash attribution.** `term.elf` is not embedded in the kernel image, so M2 and
M3 leave `kernel.bin` bit-identical to the clean build. A result recorded
against the kernel hash alone would read as two outcomes from one binary — for
anything in `user/term.c` the binding artefact is `build/term.elf`.

### Scope explicitly not taken

No ANSI/VT parsing, no colour, no cursor rendering; no resize handling (the grid
is sized once); no `SIGCHLD` reap. This is **not** ADR-024 §D5's init-driven
PRISM respawn, which remains unbuilt.

### Gates

`smoke-ctrlaltt` (shard 0, strict, 180 s) PASS. The compositor changed, so every
gate that drives it was re-verified on `0d1bcd234707e56d`: `smoke-compositor`,
`smoke-focus`, `smoke-superkey`, `smoke-modkeys`, `smoke-drag`, `smoke-mouse`,
`smoke-surface`, `smoke-agents`, `smoke-shell` — all PASS. `hygiene_check.sh`
all three PASSED; `ci-shard-check` caught the missing `gate_shards.txt` entry
first, which is the check doing its job.


## DDR-1028 — `PRADYOS_AMBIANCE_OK` does not mean the pointer is being serviced

**FIXED + measured (6/6 against a pooled ~6/14).** Kernel `aad7b4c7a2e1a776`.

The common cause behind DDR-1025, DDR-1026 and an intermittent `smoke-wmclose`.
Every pointer gate's injector waits on `PRADYOS_AMBIANCE_OK`, printed at
`compositor.c:1184` with its own comment reading *"loop is about to start"*.
There is a gap before the compositor's first `SYS_MOUSE_POLL`:
**[DDR-1029 CORRECTS THE SIZE: this paragraph originally read "~10 s", from
reading `g_ticks` buckets as wall seconds. A `SYS_CLOCK` stamp says ONE second.
The fix below is unaffected — it rests on ordering and on measured outcomes.]**

```
t=500 … t=6000   mpoll=0     <- ring 3 has not polled the pointer once
t=6000                       <- GAMMA's last geometry; window C self-closes here
t=6500           mpoll=2     <- the FIRST pointer poll
```

`smoke-mouse` survives that gap only because DDR-1026's latch holds the press.
`smoke-wmclose` cannot: its target destroys itself inside it —

```
433  last GAMMA geometry
436  PRADYOS_SURFACE_OK 2      <- already out of the live set
443  first PRADYOS_MOUSE_OK    <- the FIRST click to reach ring 3
```

so the gate reported "close box click did not close" about a window that no
longer existed, then clicked the ghost 45 more times because
`mouse_inject.sh`'s resolver reads an append-only log in which dead windows'
geometry lives forever.

**This is DDR-911's own failure returning.** Its comment already describes *"49
correct clicks hit a surface that had already gone"*, and its `GRACE_SECS 4` was
sized on *"the injector lands its click in about a second"* — an estimate taken
against a sentinel that does not mean what it looks like, so nothing in the tree
could show the number was wrong.

### The fix, in two separately measured parts

| build | kernel | `smoke-wmclose` |
|---|---|---|
| `f238169` | `56a4c4a35c92cfc5` | 1 / 4 |
| `b4c2aca` | `0d1bcd234707e56d` | 2 / 4 |
| + `PRADYOS_INPUT_READY` | `f36de18e4b2eade0` | 3 / 6 |
| + `GRACE_SECS` 4→12 | `aad7b4c7a2e1a776` | **6 / 6** |

`PRADYOS_INPUT_READY` is printed from *inside* the branch that has just polled
the pointer, so it cannot be true early. Alone it is still a coin flip: the
injector's first click and C's 4 s expiry land in the same heartbeat bucket.
`GRACE_SECS 12` is derived — a *passing* run needs 8 press edges at ~1.2 s per
injector round — and `smoke-winops` (`TIMEOUT_S=90`) still observes the shrink.
`0.43⁶ = 0.006`, so 6/6 is not the dice. The 4-second build is the mutation of
part 2 and has been run: it is the 3/6 row.

### A claim withdrawn

I called DDR-1027 a regression on this gate from **one** local pass against
**one** local fail. Three more runs each inverted it — the older build fails
*more* — and the CI failure that started the investigation was on `003dec1`,
which predates DDR-1026 and DDR-1027 both.

### Deliberately not changed

`smoke-mouse` still waits on `PRADYOS_AMBIANCE_OK`: pointing it at
`PRADYOS_INPUT_READY` would land its five clicks while ring 3 is polling and
remove the only coverage DDR-1026's press-edge latch has. The other pointer
gates are green and are left alone; the sentinel is there for any that flake.

### Not fixed, not understood

**Why the compositor takes ~10 s to reach its first pointer poll.** `mpoll` goes
2 → 161 → 767 → 1678 across successive heartbeats: the loop is running and its
early iterations are enormously slow, then accelerate. A desktop that ignores
the mouse for ten seconds after drawing itself is a real product defect. No
mechanism named, so §NON-NEGOTIABLE 3 forbids a fix. Left open.

**Residual:** `resolve_geometry()` still cannot tell a live target from a ghost.
The repair — reject a `PRADYOS_WM_GEOM` line older than the last
`PRADYOS_SURFACE_GONE` — is named and not built: it changes tooling eight green
gates depend on. Post-1.0.

### Gates

`smoke-wmclose` 6/6; `smoke-winops`, `smoke-surface`, `smoke-ctrlaltt`,
`smoke-mouse`, `smoke-drag`, `smoke-focus` all PASS. `hygiene_check.sh` ALL
THREE PASSED.


## DDR-1029 — the compositor's boot cost is 30 full-screen renders

**MEASURED + instrument armed. No fix.** Kernel `086fb267171c136b`.
**Corrects DDR-1028 §2.1 and §4.**

DDR-1028 claimed a "~10 s gap" between `PRADYOS_AMBIANCE_OK` and the first
`SYS_MOUSE_POLL`, and that the loop's "early iterations are enormously slow,
then accelerate". Both came from **reading `g_ticks` buckets as wall seconds** —
the datum was 1000 ticks between two heartbeats, converted at a nominal 100 Hz
that does not hold under TCG. A `SYS_CLOCK` stamp correlated against the
heartbeats in the same log:

```
412  [hb] t=5000
415  PRADYOS_AMBIANCE_OK                              s=19183
441  PRADYOS_LOOPSTAMP i=1 at=pre-mouse               s=19184
442  PRADYOS_INPUT_READY
455  [hb] t=6000
```

**One wall second, inside a single heartbeat interval** — and iterations 1, 2, 3
all complete within it. DDR-1028's fix is unaffected: it rests on an ordering
fact and on measured outcomes (1/4, 2/4, 3/6, 6/6), not on the magnitude.

**The real cost**, stamped per step:

```
pre-ambiance 19351  DAWN 19357  DAY 19362  DUSK 19367  NIGHT 19374  post 19379
```

28 wall seconds, all before the loop. And not five renders:
`set_ambiance(idx, frames)` (`compositor.c:990`) draws a `frames`-step OKLab
transition, so 4 announce + 1 settle = **5 × 6 = 30 full-screen 1024×768
render+present at ~0.93 s each**. Nothing about the frame loop is pathological;
the desktop takes half a minute to become ready, and every pointer gate's
injector, readiness sentinel and `surfacetest`'s self-closing window C are
sequenced against that.

**No fix.** The 24 announce renders each emit `PRADYOS_AMBIANCE <name>` and
gates assert those sentinels; cutting the renders while keeping the prints would
make every one of those assertions vacuous — the failure DDR-1012 removed from
`smoke-horizon` and DDR-973 from `smoke-fat32-multicluster`. Options named
(leave it / lower `frames` / assert on framebuffer readback); none is a one-line
change days from a release, and two of them move timing eight green gates depend
on.

Instrument bounded to 3 iterations (unbounded it would print thousands of lines
a second and slow the loop it measures — DDR-941's rule). `SYS_CLOCK`'s
one-second resolution is right for a ~28 s quantity and is recorded as too
coarse to separate iterations 1–3: a limit, not a result.

Gates: `smoke-wmclose`, `smoke-mouse`, `smoke-ctrlaltt`, `smoke-winops` PASS;
`hygiene_check.sh` all three PASSED.


## DDR-1030 — `resched FAIL ipis=0 ran=1 idle=1`: an instrument, not a fix

**INSTRUMENT BUILT + mutation-proven. NO fix; verdict deliberately unchanged.**
Clean kernel `55446cb042530e80`.

CI on `bdb41c7`, shard 3, killed `smoke-rqstress-liveness` at gate 1 of 21 —
a gate that does not own the assertion (`resched FAIL` is `GLOBAL_FORBIDDEN`,
DDR-791).

**Not this PR's:** `ran=1` means the property under test held; DDR-1004 built the
predicate and DDR-1014 already fixed one cause of this exact shape (citing CI on
`72a474a`, shard 5); and the gate boots with no virtio-tablet, so DDR-1026's
latch never runs. 3/3 non-vacuous local PASS (`resched OK`, not `SKIP`).

**The mechanism is in the code's own comment:** `idle_seen` is sampled just
before `sched_unblock` and a CPU can leave idle in between, so no kick is owed
and a correct system FAILs.

**One sample cannot settle it** — a genuinely broken kick prints the same three
fields, since the thread is then picked up by the next timer tick and still
reports `ran=1`. Turning this into `SKIP` would green the gate and delete
DDR-1014's coverage; DDR-1012 and DDR-973 each had to undo that trade.

Built: a **second idleness sample** after `sched_unblock`, identical question,
identical CPU (`self_idx` hoisted so the loops cannot drift). `idle=1 idle2=0` =
sampling artefact; `idle=1 idle2=1` = the kick was owed and missing.
**CORRECTED 2026-09-06 — DDR-1064: that second reading is WRONG.** `idle_after`
is sampled after `sched_unblock` returns and `o->idle` is live, so a CPU can
ENTER idle between the kernel's kick loop and the sample; DDR-1030 closed
DDR-1004's window and opened its mirror image. Read `kidle=`/`kkick=` instead —
recorded by `sched_unblock`'s own loop, at the instant it ran, on the TCB.
Mutation-proven by forcing the FAIL branch (kernel `234adcfec677a702`):
`ipis=1 ran=1 idle=1 idle2=1`.

Closes nothing: no fix, no rate bound. Gates: `smoke-rqstress-liveness` 2/2 PASS;
`hygiene_check.sh` all three PASSED.


## Release candidate re-verified on the current tip (2026-08-31)

The RC evidence in §CURRENT BUILD STATE was recorded against `ace232f`. The tree
has moved **43 commits** since, so it was stale. Re-run on kernel
`55446cb042530e80`:

- `smoke-iso-x86` **PASS** — BIOS arm OK and UEFI arm OK, one ISO, both boot
  paths, same sentinel.
- `smoke-iso-userspace` **PASS** — the ISO boots a live OS: SFS root + PRISM +
  AETHER agent + write/read/delete round-trip.

ISO 52,805,632 B. `hygiene_check.sh` all three PASSED; `smoke-shell` 5/5.

PR #17's merge conflict against `dev/phase1` (`fa29506`) is resolved in merge
commit `35291db` — documentation only, kernel bit-identical before and after.


## DDR-1031 — `SYS_MPROTECT` (NSI 97)

**IMPLEMENTED + GATED + mutation-checked.** Kernel `0bf4d1df5502b2cb`,
`-Werror` clean. Group D. Gate `smoke-mprotect` (shard 5, strict, 120 s).

Adds `vmm_protect_range` — the primitive the vmm did not have. `vmm_map`,
`vmm_unmap`, `vmm_resolve` and `vmm_user_range_ok` all existed; nothing changed
a mapping's permissions while **keeping its frame**.

**The trap.** A PTE here carries `PTE_SW_COW` (0x200) and `PTE_SW_SHARED`
(0x400). Rebuilding it as `frame | flags` clears both: that breaks DDR-1003's
shared-frame invariant, and makes `vmm_cow_fault` return early
(`vmm_cow.c:115` tests exactly that bit) so the page is **never copied**. Both
bits and the cache attributes are preserved verbatim.

**Three refusals, each with a reason.** `PROT_WRITE|PROT_EXEC` → `-EACCES`
(W^X, DDR-757). `PROT_WRITE` on a COW page → `-EACCES`: the hardware RO bit *is*
the copy trigger, so granting write would let one process write a frame another
still shares, with no copy and no fault — removing write is allowed.
`PROT_NONE` → `-EINVAL`: an absent user page collides with ADR-038's
demand-paged stack, which faults absent pages *in* rather than reporting them.
Two-pass, so a partly-unmapped range changes nothing.

**The probe forks** because a write to a read-only page is fatal — the child
takes the fault and the parent reads `st=-1` from `wait4` (a ring-3 fault is
`sched_exit(-1)`, `idt.c:703`), versus an explicit `exit(7)` if the store had
succeeded. Two distinct values, so "enforced" is never inferred from silence.
And it protects **before** forking on purpose: `fork` COWs only *writable*
pages (`vmm_cow.c:87-92`), so forking first would have made the child's store a
COW fault that succeeds — arm B would report "enforced" on a kernel with none.

| mutant | change | kernel | outcome |
|---|---|---|---|
| — | clean | `0bf4d1df5502b2cb` | **PASS**, five arms |
| M1 | drop the software-bit preservation | `d7dce7a13f82d86c` | **FAIL at arm E only** |
| M3 | drop the W^X refusal | `e1239532af6f99db` | **FAIL at arm D only** |
| M2 | drop `invlpg` | `a5b1e4dbd1107888` | **PASSED — no arm caught it** |

**Arm E was missing from the design,** and the original M1 plan was unrunnable
for the same reason it was proposed: `smoke-cowfork` cannot see a dropped COW
tag, because `vmm_protect_range` is reached only through `mprotect`. The fix is
the asymmetry above — RO on a COW page is allowed, RW refused — so a dropped tag
turns `-EACCES` into `0`.

**M2 passed every arm, and the design's prediction for it was wrong.** Arm B is
decided by the *child's* page tables, not the parent's TLB; arm C's write
succeeds under a stale *writable* entry too. The `invlpg` is **uncovered**, and
cannot be covered by a probe of this shape: a missing invalidation is only
visible as a write that should have faulted and did not, and the observer dies.
There is no `SIGSEGV` handler on this path. Recorded as an uncovered line, not
claimed as tested.

Gates: `smoke-mprotect`, `smoke-cowfork`, `smoke-sysmmap`, `smoke-shell` all
PASS; `hygiene_check.sh` all three PASSED.


## DDR-1032 — `execve` argv/envp marshalling

**IMPLEMENTED + GATED + mutation-checked.** Kernel `64aaecffaeeb2800`,
`argtest.elf` `4e42203e5c9a5518`. Gate `smoke-execve-argv` (shard 6, strict).

`sys_execve` read `uargv`/`uenvp` and threw them away with a `(void)` cast, and
`elf_build_image` hardcoded `argc=1, argv[0]=path`. So `execve` with arguments
**succeeded and delivered none** — the shape DDR-877 called "worse than
incomplete" for `mmap`'s dropped `fd`/`offset`.

**Ordering is the design.** The strings live in the caller's address space,
which `execve` is about to destroy, so they are copied into a kernel blob before
`elf_build_image` and long before the CR3 switch. Hence a flat blob rather than
a pointer array. Bounds: 32 entries, 1024 bytes, `-E2BIG` past either (a new
errno; POSIX's code for this was absent).

**Backward compatibility is structural:** `args == NULL` takes the original code
verbatim, and every existing caller passes NULL — `elf_load` for ~65 boot probes
and PRISM's `run`. `smoke-shell`, `smoke-sysexec`, `smoke-user` confirm it.

**The receiver is assembly on purpose:** `argc`/`argv`/`envp` arrive on the stack
at entry, and every C probe carries `force_align_arg_pointer` (DDR-823), which
realigns RSP — `__builtin_frame_address` cannot find `argc`.

**The alignment arm caught a bug in this DDR's own code.** The frame is 7 fixed
slots plus one per string; RSP is 16-aligned only when that total is even. The
first draft padded on **even** — exactly inverted — and shipped a misaligned
stack for `argc=3, envc=1`. An assembly receiver cannot feel that, so all six
other arms passed. `PRADYOS_ARGV_ALIGN` measures RSP at entry and read `bad`.

| mutant | change | kernel | outcome |
|---|---|---|---|
| — | clean | `64aaecffaeeb2800` | **PASS**, seven arms |
| M1 | `sys_execve` discards argv again | `b18b9276d19e5497` | FAIL — `ARGC=1`, `argv[0]=/ARGTEST.ELF` |
| M2 | invert the alignment pad | `311f76bf9d6cf990` | FAIL at the alignment arm only |

**Two hygiene catches.** `ci-probe-rodata-check` rejected the first
`argvtest.elf` — `static const char *argv[]` holds addresses and lands in
writable data, which these single-R+X-segment probes cannot have; built on the
stack instead. And `argtest.asm`'s first draft staged a digit in `.data` and took
a `#PF err=0x7`.

**A latent build defect, fixed.** `USER_ALL_SRCS` listed `user/*.c` and
`user/*.h` but **not `user/*.asm`**, so editing any assembly probe rebuilt
nothing — including the `incbin`'d ones whose content genuinely changes
`kernel.bin`. Measured: an edit to `argtest.asm` left the kernel bit-identical
and the gate re-ran the old image, reproducing an already-fixed fault.

Gate count 165 → **166**. Gates: `smoke-execve-argv`, `smoke-shell`,
`smoke-sysexec`, `smoke-mprotect` PASS; `hygiene_check.sh` all three PASSED.


## DDR-1033 — `SYS_IPC_SEND` / `SYS_IPC_RECV` (NSI 98/99): the ring-3 IPC door

**IMPLEMENTED + GATED + mutation-checked.** Kernel `715520928e873aab`.
Gate `smoke-sendipc` (shard 7, strict). Closes the `ACTION_SEND_IPC` gap
DDR-1017 recorded as blocked. Built on operator instruction, PR #17.

DDR-1017 was right that there is no ring-3 door, and understated how much
already worked: `ipc_send`/`ipc_recv` are complete **and already
capability-gated** (`CAP_IPC_SEND` bit 7 / `CAP_IPC_RECV` bit 8 exist and are
checked inside them), and `ipc_recv` already carries DDR-961's bounded form.
The genuinely missing pieces were the **door** and the **addressing** — nothing
let one agent name another's roster slot.

Addressing is the roster slot index, the same identifier `SYS_AGENT_ROSTER`
already uses, so no new namespace is invented.

**Two layers, and an honest limit on the second.** `is_ipc` on `struct tcb`
answers "may this process use the door at all" — kernel-set at spawn, never
mintable, explicitly zeroed in `sched_create`. The capability handle is minted
beside it. But every slot endpoint shares **one** `res_id`, so the capability
grants "IPC at all", **not** "send to slot 3 but not slot 5". Real, but coarse;
per-slot `res_id`s are the extension if policy is ever wanted.

| mutant | change | kernel | outcome |
|---|---|---|---|
| — | clean | `715520928e873aab` | **PASS**, five sentinels |
| M1 | drop the `is_ipc` check | `8853aecb812532ba` | FAIL at arm B — both processes print `rc=0` |
| M2 | copy one word instead of four | `5d1805213ae89c84` | FAIL at arm C |
| M3 | widen the slot bound past the array | `484e9b390aed1d7a` | FAIL at arm D |

**Arm B was passing for the wrong reason, and the first M1 proved it.** The deny
process was originally spawned with neither the flag nor the capability, so a
mutant that defeated `is_ipc` *still* produced `rc=-1` — `cap_authorize` refused
it anyway. `is_ipc` could have been deleted outright with the gate still green.
Seventh dead-arm instance, and the **first found by a mutant rather than by
reading**. Fixed by spawning the deny process with `ipc_grant()` and then
clearing `is_ipc`, so it holds the capability and lacks only the door.

The lesson worth carrying: two independent checks in series each mask the
other's absence, and a fixture that trips both at once cannot tell you which one
is load-bearing.

M3 is also worth reading: with the bound widened, a ring-3 integer indexes
`g_agent_ep[99]` directly. The slot check is the only thing between userspace and
an out-of-bounds array read.

**Not done:** the AETHER action path does not yet *call* this, so an approved
`SEND_IPC` still has no automatic effect. Recorded in the pre-launch checklist.

Gate count 166 → **167**. NSI max 97 → **99**. Gates: `smoke-sendipc`,
`smoke-aether`, `smoke-agents`, `smoke-shell` PASS; `hygiene_check.sh` all three.

---

## DDR-1034 … DDR-1053 — index (recovered 2026-09-03)

**Why this block exists.** §NON-NEGOTIABLE 11 requires this file updated in the
same commit as the code. It was not, for **twenty consecutive DDRs** — 1034
through 1053 had no entry here at all, while their DDR files, gates and code all
shipped. That is a real defect in the project's memory rather than a cosmetic
one: six of the twenty are **assessed-and-not-built decisions**, and a decision
nobody can find in the tracker is a blocker the next session re-derives from
scratch. Recorded compactly — one line of finding plus the gate — with the DDR
file as the authority for detail. Recovered as an index, not rewritten as prose,
because inventing twenty retrospective narratives would be worse than a pointer.

| DDR | finding | gate |
|---|---|---|
| **1034** | `SYS_RUN_EXPERIMENT`/`SYS_EXP_RESULT` (NSI 100/101) + a **bounded** stack machine (no LOAD/STORE/DIV by design). §4 first said "bounded, preemptible" and that was **wrong**: `MSR_SFMASK` clears `RFLAGS.IF` for the whole syscall, so nothing preempts it and `EXP_MAX_STEPS=4096` is load-bearing. | `smoke-runexp` |
| **1035** | **CI builds once.** Ten shards each compiled the same kernel; a `build` job now does it and shards `sha256sum -c` the binary **before and after** the gates. That assertion earned itself three times, printing `kernel.bin: OK` on red shards so "the gates were red" and "the shard ran a different binary" stayed separate findings. | CI workflow |
| **1036** | **Ghost windows** — the compositor never announced a window's *destruction*, so `mouse_inject.sh` resolved geometry from an append-only log and clicked a surface already gone. `PRADYOS_WM_GONE` keyed on **id, not poll index**. §5's claim that `smoke-wmclose` already covered this was **wrong and the DDR corrects itself**. | `smoke-ghostclick` |
| **1037** | `SYS_POLL` (NSI 102) — **generalised** the readiness predicate rather than duplicating it (`fd_ready_mask()` shared with `epoll_wait`). Deadline computed **once** before the loop. Arm E nearly shipped vacuous. | `smoke-poll` |
| **1038** | `SYS_FUTEX` **ASSESSED and NOT BUILT.** A futex is a shared-memory word and this kernel has no way for two threads to share one — no `CLONE_VM`, no `MAP_SHARED` file backing, and fork COWs everything writable, so the two sides would wait on **two different physical words**. Buildable the moment either lands. | — |
| **1039** | **PRISM erase.** `readline()` appended every non-newline byte, so a backspace landed *in* the buffer. Invisible to all 170 gates because every gate injects byte-perfect lines. The DDR's own first arm was **vacuous** and it says so. | `smoke-shell` (erase arm) |
| **1040** | **SMEP** + the one-shot expected-fault latch. **The vacuity trap was measured before a line was written:** the TCG default `qemu64` reports `smep=false`, so a correct implementation would have been a permanent no-op. M3 **passes every arm** — the RIP-window check is measured-uncovered. | `smoke-smep` |
| **1041** | **SMAP.** The enumeration was **measured, not grepped** — SMAP on with no `stac` anywhere; the `uaccess.h` contract HOLDS. **Not fixed and named:** an interrupt inside the window runs with **AC still set**. | `smoke-smap` |
| **1042** | `smoke-resizeall`'s checker **failed arm e using a record arm w produced** — worse than a flake, a specific plausible **false accusation** against a correct subsystem with a real log line behind it. `check()` split into `invariant()` (every record) and `liveness()` (at least one). | `ci-resizecheck-selftest` |
| **1043** | **The silent-panic instrument was never armed, and would have been corrupted if it were.** `QEMU_QMP_DIAG` was set by nothing in the tree; and the dump was appended to `$SERIAL_LOG`, which QEMU holds open **without `O_APPEND`**, so the guest's next output overwrote it. Sidecar + narrowed arming. | CI workflow |
| **1044** | **`#MC`.** The row was half right — `idt.c` already knew vector 18; **the real defect was upstream: with `CR4.MCE` clear a machine check raises no exception at all.** M2 landed on arm A not D, hence M3, which fails arm D alone. | `smoke-mce` |
| **1045** | A **markdown-only commit reddened the aarch64 job** — a vendor repo 403 in the runner image. **Two failed fixes, and the pattern is the point:** both asserted a claim about the runner image that could not be checked. The shipped fix **assumes nothing** — tolerate a failing `update`, then *prove* the index usable by resolving. | `ci-aptprepare-selftest` |
| **1046** | **Kernel text was writable through the identity alias**, and **the audit could not see it** — the verdict loop walks only the higher-half PTEs, so it printed `W^X OK` on a kernel with writable text. M1 **is** the pre-fix tree. | `smoke-wxkernel` |
| **1047** | **`lock_stat`** — contention counts and wait time; **hold time deliberately dropped** (an `rdtsc` pair on the hottest primitive could *move* OPEN-2 rather than measure it). `g_sched_lock` dominates by **~450x**. **Not gated, deliberately.** **Cannot see `mnt_lock`**, the OPEN-1 route 1 prime suspect. | none (by design) |
| **1048** | A **local** way to check a runner-environment assumption. The finding reframes the task: **this host is already Ubuntu 24.04.4** and the file refuting DDR-1045 attempt 1 was on this disk unread. Found a real defect on first use — a `grep -q` **SIGPIPE race** under `pipefail` marking a resolvable package missing. | `ci-runnerenv-selftest` |
| **1049** | **A lone silent panic left a green run**, and the field built to name it could not print: `panic_stage` was gated behind `g_panic_extra`, which increments **only in the loser branch**. Every channel empty, gate sentinels all present, **run green with a panicked CPU in it.** `panic_stage=` added to `GLOBAL_FORBIDDEN`. | `smoke-selftest` |
| **1050** | **I/O APIC stage D ASSESSED and NOT BUILT.** Most of it exists (DDR-874). **Blocker: there is no ACPI `_PRT` parser**, so the three PCI INTx fallbacks cannot be routed correctly, and a wrong GSI looks exactly like a dead device. And **the benefit is already delivered** — MSI-X provides affinity for every device carrying real traffic. | — |
| **1051** | **KASLR ASSESSED and NOT BUILT** — a **sequencing judgment**, not a blocker. Zero relocation sections, so a virtual slide needs a relink *and* an applier. The deferral reason is that KASLR **degrades the one diagnostic discipline every open defect depends on**: `[apfreeze]` has three producers told apart *only* by RIP. | — |
| **1052** | **Keccak-f[1600] / SHA-3 / SHAKE** — the ML-KEM/ML-DSA prerequisite. Constants **derived and proved**, not transcribed: **the first generator produced `RC[0]=0x03` instead of `0x01`**. Vector selection is the design — M2 **passes vectors 1–6 and fails 7**. | `smoke-shake` |
| **1053** | **FIPS 204 ML-DSA vectors acquired — the predicted blocker does not exist**, correcting DDR-1052 §7. Both premises hold, the conclusion does not: NIST publishes ACVP in **its own GitHub repo**, which is reachable. Provenance is a **committed tool**, and re-running it regenerates the headers **bit-identically**. | — |


## DDR-1055 — a required gate sentinel was spliced by another CPU's printer

`smoke-nethammer` had been red on CI shard 3 four times, every one of them on a
commit that provably cannot change `kernel.bin` (`kernel.bin: OK` from DDR-1035's
hash assertion on the red shard). Reproduced locally 1 in 3 on an idle machine.
Root cause, from `build/gatelogs/nethammer.log.fail-3786`:

```
[user] ELF loaded (embedded); net hammer spawned=PRADYOS_SOVEGRESS_AUDITED
```

`kernel/main.c:1836` built the required sentinel `net hammer spawned=2/2` from
THREE unlocked console calls, and a ring-3 probe's `write(2)`
(`user/sovegresstest.c:82`) landed between two of them. Every character the
kernel meant to print is on the wire; the line a gate can match is not. That is
why every red capture had unbroken heartbeats, no panic and no `[apfreeze]`.

**`console_line_lock()` did not cover this, although `console.h` presents it as
the answer.** It excludes only other holders of `g_line_lock`, and `kwrite` — the
ring-3 `write(2)` path and the busiest printer in the system — never took it.
Four sites in the whole tree held that lock. The DDR-1046 shape: a control that
cannot see the case it exists for reads exactly like a control that works.

**Fix: one `kwrite`, not one more lock.** `kline` (`kernel/console.{h,c}`)
assembles the line into a stack buffer and emits it with a single `kwrite`, which
holds `g_console_lock` for the whole buffer — and every printer in the tree takes
that lock, so the line is atomic against all of them. Locking the composite
instead would still leave it open to any bare `kputs` from another CPU.

Rejected deliberately: making `kwrite` take `g_line_lock` (an IRQ-masked
acquisition on the hottest output path, contending with the timer ISR — the cost
DDR-1047 refused for the same OPEN-2 reason), and a per-CPU recursion guard (it
needs a CPU id at every `kputs` from the first boot print; `lapic_id()` is invalid
before the LAPIC is mapped and the GS-based id is what DDR-1010 caught being
*wrong*, so a broken GS would corrupt the console exactly when it is the last
diagnostic left).

**Scope measured, not read.** All 268 `EXTRA_SENTINEL` patterns were asked one
question — does any single string literal in the tree contain it? 186 do (safe);
82 do not, and of those **16 are ring-0 composites**, every one confirmed *not*
inside a `console_line_lock` region by a source-order depth scan. 21 sites
converted, including the four AP announces in `smp.c` whose line lock is removed
as ineffective. `kernel/idt.c:748` is deliberately untouched: it uses
`console_line_trylock` and prints anyway, because a trap printer that blocks
turns a diagnosable fault into a hang.

Overflow is loud, not silent: `[kline] TRUNC` is emitted on overflow and added to
`GLOBAL_FORBIDDEN` — inserted BEFORE the final list line per §NON-NEGOTIABLE 6, so
the documented verification command's `sed` terminator did not move (74 → 75,
CLAUDE.md's stated count updated in the same commit).

**Measured.** Campaign: `smoke-nethammer` **20/20 PASS** on one binary
(`5f0a2f60d56fbd9b`, `kernel_after == kernel`), every capture non-vacuous (47
`[hb]` lines), `sentinel_intact` 20/20, `spliced` 0/20 — `(2/3)^20 = 3.0e-4`
against the pre-fix 1-in-3. Regression: **18/18 gates rc=0**, one per converted
print site plus the hygiene gates, with `smoke-selftest` load-bearing because
this edits `GLOBAL_FORBIDDEN`. CI on `bf784f7`: **both suites green**, including
shard 3 where `smoke-nethammer` runs.

Gate count unchanged. `kernel.bin` 1,192,330 → **1,196,426 B** against the
1,572,864 B gate. Zero warnings at `-Werror`; `hygiene_check.sh` ALL SIX.

**Also found and NOT fixed here — the same defect one ring out (DDR-1055 §9).**
Gates assert in two places, and the sweep above covered only the first: the
Makefile post-check `grep`s match the *whole* measured line, and ring-3 probes
build those from many `wr()` calls — `user/actiondeltest.c:172` uses nine,
which is **eleven** `write(2)`s, because `wrdec` emits one digit per write, so
`id=258` costs three on its own (measured in DDR-1056). That is very likely (**not established**) the
`smoke-actiondel` "no measured line in the capture" failure that landed on the
same docs-only commit as the third nethammer timeout. Fixed separately, with its
own gate evidence.

---

## DDR-1056 — the same splice one ring out: ring-3 measured lines, and musl's two-iovec fflush

DDR-1055 asked all 268 `EXTRA_SENTINEL` patterns whether any single literal
carried them, and fixed the ring-0 composites. That sweep had a hole: **a gate
asserts in two places**, and the second is the Makefile recipe's post-check
`grep`, which matches the *whole* measured line.

`PRADYOS_ACTIONDEL_OK id=` does sit in one literal, so the sweep called it safe.
The post-check does not care: `user/actiondeltest.c:172` builds that line from
nine `wr()`/`wrdec()` calls, and **that is eleven `write(2)`s**, because `wrdec`
emits one digit per write — `id=258` costs three on its own. Eleven console-lock
acquisitions, **ten gaps**, and the wider the number the more gaps.

**Three distinct splice paths, all measured from this tree, not recalled:**

1. Probe lines built from many `wr()` calls (the actiondel case, plus
   `actionhypo`/`actionquery`/`actionread`/`actionspawn`/`fat32mc` and the
   `polltest`/`exptest`/`ipctest` measured lines).
2. **musl's `fflush` emits TWO console writes, not one.** `__stdio_write` always
   passes two iovecs — the buffered bytes and the new bytes — and `sys_writev`
   calls `fd_write_user` per iovec, i.e. a separate `kwrite` and a separate
   console-lock acquisition each. Every musl-linked program is affected: the
   compositor, PRISM, `term`, `cmusl`, `agent_base`, `init`. (stdout is *fully*
   buffered here, checked in-tree: `__stdout_write` falls back to `lbf = -1`
   when `ioctl(TIOCGWINSZ)` fails, and this kernel registers no `SYS_IOCTL` at
   all — so bytes do accumulate between flushes and the two-iovec case is the
   normal one.)
3. `fd_write_user` chunks the console at 256 bytes. Recorded, not fixed.

**Fix.** `user/include/uline.h` builds the line and hands it to the probe's own
`wr()` — one call, one write. `sys_writev` on an `FD_CONSOLE` fd gathers into one
buffer and issues a single `kwrite`. **The gather is 256 bytes, deliberately
`fd_write_user`'s own chunk size and not musl's 1024-byte `BUFSIZ`:** a larger
buffer would hold `g_console_lock`, and therefore interrupts, across four times
today's maximum UART busy-wait on the hottest output path, which is exactly the
cost DDR-1047 refused near a timing-sensitive AP freeze. At 256 the masked
window is unchanged and every measured line still fits (the longest is ~90 B).

**Proved deterministically, because there is nothing to race.** Unlike DDR-1055
this defect does not reproduce locally, so a rate campaign would measure nothing.
What the fix changes is countable, and the kernel already counts it — DDR-948's
`writes=` on the `sys_exit` line. The actiondel probe went **13 -> 3**: exactly
-10, eleven writes for the measured line collapsing to one, its two other writes
unchanged. Same gate rc=0 and the same measured line on both sides.

**Regression 16/16 rc=0**, `kernel_after == kernel`. `kernel.bin` 1,196,426 ->
**1,204,618 B**. `GLOBAL_FORBIDDEN` 75 -> **76** (`[uline] TRUNC`).

**Also fixed in passing — a gate diagnosability defect.** `smoke-actiondel`'s
failure path printed only `no measured line in the capture` and dumped nothing,
which is precisely why the `c8c93ed` CI log cannot settle whether that failure
was this defect: a spliced line and a probe that never ran look identical from
outside. It now dumps what it found, as `smoke-nethammer` already did — which is
why DDR-1055 was diagnosable at all.

**NOT proven, and named rather than glossed:** the `sys_writev` gather has no
counter of its own (`dbg_writes` increments once per `writev` however many
iovecs it fans out to), and no mutation covers it — a mutant reverting it would
pass every gate, because the split it reintroduces is invisible unless a race is
won. The `smoke-actiondel` and `smoke-surfclose` CI failures remain
**unattributed**; what changed is that the next occurrence is decidable.

---

## DDR-1054 — ML-DSA-44 (FIPS 204) key generation, IMPLEMENTED + gated + M1/M3

Step 3 of Post-Quantum Security, which CLAUDE.md §PHASE 3 makes **mandatory v1
scope, landing before the ISO**. DDR-1053 closed with "NO ML-DSA IMPLEMENTATION
SHIPS IN THIS CHANGE". One does now.

**keyGen only, and byte-exact against NIST's OWN ACVP vectors.** keyGen is
DETERMINISTIC — one 32-byte seed maps to exactly one `(pk, sk)` — so a mismatch
is unambiguous. The alternative, a sign-then-verify round trip, passes on **any**
self-consistent wrong implementation: the dead-arm class, which this project has
hit nine or more times.

**NO MAGIC CONSTANTS.** The 256 twiddle factors are computed as
`zeta^brv8(i) mod q` from `zeta = 1753`, and the inverse-NTT scale as
`256^(q-2) mod q` by Fermat — **which evaluates to 8347681, the literal the
reference implementations carry.** That equality was checked, not assumed. This
is DDR-1052's lesson applied: its first hand-rolled Keccak constant generator
produced `RC[0] = 0x03` instead of `0x01`.

**All mutable state is caller-owned** (21,288 B scratch, derived tables and
selftest buffers included; not one `static` mutable object). Load-bearing twice:
it delivers the reentrancy `mldsa.h` always claimed, and `user/user.ld` gives
each probe a single R+X PT_LOAD, so any writable allocated section would link
cleanly and **fault on its first store** (DDR-826 — how `smoke-ed25519` once
failed with "sentinel not found", which reads as "the crypto is wrong" and was
nothing of the kind).

**THE KATs DO NOT COVER Power2Round's BOUNDARY, and that was measured.**
Mutant M3 (`r0 > 2^(D-1)` → `>=`) **passed the ACVP arm outright**: `r0` equals
`2^(D-1)` exactly in **0 of the 2048 coefficients** across both vectors, against
~0.125 expected hits per key. A direct 10-case boundary arm was added; it
reports a **negative** index so a boundary failure can never be read as a vector
failure. M4 (`s1`/`s2` index base `L` → `K`) also passed and is an **equivalent
mutant, not a gap**: at ML-DSA-44 `k == l == 4`.

**Two-sided proof.** The host harness links the SHIPPED `mldsa.c` and reproduces
`tools/ci/mldsa_ref.py` byte-exactly on a seed in no vector file. On the running
OS, `smoke-mldsa` (shard 0, strict) prints
`PRADYOS_MLDSA44_KEYGEN_OK acvp=2 p2r=10` — read back out of a 431-line capture,
because rc=0 alone is worthless (DDR-1041). Mutants on the **shipped kernel**:
M1 (`zeta` 1753→1754, `8e7a4ac795c9e71f`) and M3 (`4923b1e5d7af82de`) both
redden the gate on **different arms**; reverting returns `bd921648b60ae930`
bit-for-bit. Counts are **reported by the probe**, not literals, and a
`_Static_assert` on the table size means a regenerated header cannot silently
shrink coverage.

Regression 8/8 rc=0, `kernel_after == kernel`. Gate count 174 → **175**.
`kernel.bin` 1,204,618 → **1,229,194 B** against the 1,572,864 B gate. Zero
warnings at `-Werror`; `hygiene_check.sh` ALL SIX.

**NOT CLAIMED:** no `sigGen`/`sigVer`, so nothing here is post-quantum
*authenticated* yet and the audit ledger still uses SHA-256; ML-DSA-44 only;
nothing about constant-time behaviour; and **the kernel does not contain
ML-DSA** — `mldsa.c` is compiled into the ring-3 probe only, exactly as
`keccak.c` also is for `shaketest`, because nothing needs it until the ledger
does.

---

## DDR-1057 — ML-DSA-44 Sign_internal (FIPS 204), IMPLEMENTED + gated + S1/S4

DDR-1054 shipped keyGen and closed with "nothing is post-quantum *authenticated*
yet". Signing exists now.

**The predicted blocker did not exist, for the third time.** FIPS 204 signs with
a random `rnd` by default, and a randomized signature can only be checked by
verifying it — which passes on any self-consistent wrong implementation. But
ACVP publishes **deterministic** groups (`rnd` = 32 zero bytes), and among them
`signatureInterface: internal, externalMu: false` — `ML-DSA.Sign_internal`
itself, no message-encoding wrapper, no pre-hash, so a mismatch localises to the
signing algorithm. `tools/ci/mldsa_sign_ref.py` (Python oracle, byte-exact vs
ACVP) was written and verified **before** any C — the DDR-1052 discipline.

Re-running the fetcher regenerated the **keyGen header bit-identically**, which
is the property a provenance tool should have and is worth checking.

**Vectors chosen, not taken:** tcId 110 (message **1 byte**) and 118 (**273
bytes**). Shortest-first because the headers are large, but not only shortest: a
set whose messages all fit one SHAKE block never exercises multi-block
absorption in `mu = H(tr || M)`.

**TWO branches the KATs do not cover, both measured.** `Decompose`'s
`lo == GAMMA2` occurs in **0 of 28,672 calls** (~0.15 expected), so mutant S4
(`>` → `>=`) **passed the KAT arm** — the DDR-1054 `Power2Round` shape in a
different function; fixed with 12 direct FIPS 204 Alg. 36 cases including
`r = q-1` for the special branch. And the `hint_total > OMEGA` rejection:
defeating it still passes, because for well-formed keys the hint weight never
nears 80. That one is **not fixable by a unit arm** — reaching it needs a crafted
secret key, which is not a known-answer test — and is recorded uncovered. What
was done instead is a memory-safety guard: that check is the only thing keeping
`HintBitPack` inside the 2420-byte signature, so the encoder now re-checks the
bound **at the write** and returns -1. An untested guard on a fixed-size buffer
write is worth not relying on.

Rejection loop **bounded at 1000** (expected ~4.25) — FIPS 204 states no cap and
an unbounded loop is an S2 violation here (DDR-961/994).

S1 (`rnd` non-zero, `ab06e1593d5ea1c5`) and S4 (`0d3f5e7782071cef`) redden
`smoke-mldsa` on **different arms**; revert returns `9d3a813c3910ac1f`
bit-for-bit. Regression 8/8 rc=0. `kernel.bin` 1,229,194 → **1,249,674 B**.
Gate window 120 → 180 s.

**NOT CLAIMED:** no `sigVer` — this OS can produce a post-quantum signature and
cannot yet check one; no application (ledger still SHA-256), and the kernel does
not contain ML-DSA (the probe does); deterministic mode only; and **nothing
about constant-time behaviour**, which matters most for signing — the reduction
is a 64-bit `%` and the rejection loop's iteration count is itself
secret-dependent, so this must not be used against an adversary who can measure
it until that is addressed.

---

## DDR-1058 — ML-DSA-44 Verify_internal: the primitive set is COMPLETE

keyGen (DDR-1054) + sign (DDR-1057) + verify (here), all against NIST's own
ACVP vectors, all gated by `smoke-mldsa` (shard 0, strict).

**The verify vector set is mostly NEGATIVE, and that is the point.** ACVP's
ML-DSA-44 `Verify_internal` group is **3 signatures that must verify and 12 that
must not** — so an implementation that always answers "valid" passes every
positive test, which is sign-then-verify in disguise. Both verdicts are pinned
and the generator refuses a one-sided set. V1 (always accept) fails at vector 3;
V2 (always reject) fails at vector 1.

**Coverage measured per guard, not assumed.** Hint-encoding validation catches
tcIds 107/113/119 (V4 fails), real coverage of the checks that stop a verifier
being fed arbitrary bytes. **The `||z||inf < gamma1 - beta` bound catches none of
the twelve.**

**THE FINDING: that bound cannot be covered by a synthetic vector either, and it
was measured before the test was written.** Forging an out-of-range `z` gives
`verify WITH bound = False, WITHOUT = False` — altering `z` changes `w1'` and
therefore `c~'`, so the hash comparison rejects it either way, and a test
asserting "rejected" would pass on an implementation with **no bound check at
all**. Dead-arm class, second time caught in *design* rather than after shipping.
Recorded measured-uncovered with the reason.

**UseHint's `r0 == 0` boundary** is likewise unreached (V5 passes all five) — the
third function in a row after Power2Round and Decompose, so these direct arms are
now routine. 13 cases from FIPS 204 Alg. 40.

Own scratch (27,400 B), not signing's under different names. V1
(`6d026ed03832b65c`) and V5 (`336448d366164153`) redden the gate on different
arms; revert `46016bc8c7c7fa3b` bit-for-bit. **Regression 9/9 including
`smoke-iso-x86` (BIOS + UEFI)**, which matters because `kernel.bin` is now
**1,278,346 B** against §INV.18's 1.5 MiB load window (294,518 B headroom).

**NOT CLAIMED:** no application — the ledger is still SHA-256 and nothing calls
these in anger; the primitive set is complete, the *use* of it is not built. The
`||z||inf` bound is untested and is a real FIPS 204 step. Internal interface
only; ML-DSA-44 only.

---

## DDR-1059 — the ML-DSA-signed ledger: ASSESSED and NOT BUILT, blocker named

§PHASE 3's first-named PQC application. **Not built, deliberately**, per that
section's own instruction to name the exact blocker rather than ship filler.

**Every candidate blocker was measured so none can be offered later:** the crypto
is built and gated (DDR-1054/1057/1058); keygen **0.26 ms**, sign **0.39 ms**,
verify **0.27 ms**; `mldsa.o` ~60 KB against **294,518 B** of `kernel.bin`
headroom; and the correct shape is ONE signature over the chain head, since
`chain[i] = SHA-256(chain[i-1] || fields)` already binds the whole log.

**THE BLOCKER IS KEY CUSTODY.** A signature beats a hash only if the adversary
lacks the private key and the verifier learns the public key independently of the
artefact. Neither holds: `grep` finds no TPM, no PCRs, no secure boot, and
`kernel/syscall/sys_vault.c:22` holds `g_owner_seed` as **32 literal bytes in the
image**, which `ags_sign` signs with today. Anyone holding the ISO holds the
private key. A signed ledger could be edited, re-chained, re-signed, and would
verify — no assurance gained, considerable assurance *implied*. The DDR-1046
shape.

**This also kills the consolation prize:** swapping AGS's Ed25519 for ML-DSA is
equally empty, because an attacker with the key never needs to break the scheme.

**Three routes named**, cheapest last: a hardware root of trust; first-boot keygen
into a protected store (circular until the first exists); or out-of-band
publication of the public key at install time — buildable in an afternoon, a
narrower and honest claim, and a **decision** (DDR-793 class) rather than a task.

**Release-note wording is given explicitly**, because the difference matters:
"post-quantum signature primitives, NIST-vector-verified, and a tamper-evident
audit chain" is true; "post-quantum signed audit ledger" would be false.


---

## DDR-1060 — `lock_stat` recorded only CPUs that eventually ACQUIRE

**IMPLEMENTED + M1/M0 forced proof. No gate, deliberately.**

`spin_lock_contended()` ran the spin loop **first** and claimed its slot
**second**, so every counter was written only after the lock was acquired. A
wedged AP — which by definition never acquires — contributed **nothing**. The
instrument DDR-1006 §7 names as OPEN-2's next step was structurally blind to
frozen CPUs, and `lock_stat.h`'s own header states the purpose it could not
serve. It measured **completed** waits: the complement of the set it was built
for. Dead-arm class, and the first instance found in a **diagnostic** rather
than a gate.

**DDR-1047's M1 could not have caught it** — that forced proof ran on a
*healthy* boot (t=5000, nothing frozen), where every waiter eventually acquires
and the table looks exactly right. **A diagnostic proven only on the healthy
path is proven only for the healthy path.**

**Second half, in the printer:** `if (!hits) continue;` would have skipped a
lock whose only interaction is a CPU stuck on it (`hits == 0`). Both halves had
to move together or the fix is decorative.

**Fix:** claim the slot and increment a live `waiters` count **before** the
spin; decrement after acquiring. A frozen CPU leaves a permanent **+1 on exactly
the lock it is stuck on**. Per-**lock**, not per-CPU, and that is the design: a
per-CPU `waiting_on` needs to know which CPU is running, and both routes are
documented hazards *on this path* — `this_cpu()` reads `%gs:0` and DDR-1010
caught a broken SWAPGS discipline as one of OPEN-2's own producers, while
`lapic_id()` is invalid pre-LAPIC (DDR-1055's reason for refusing a per-CPU
console guard). The `[apfreeze]` line already prints `cpu=`/`rip=`/`bt=`.

**`mnt_lock` is now visible** (`lock_wait_begin/end`) — PRE_LAUNCH_CHECKLIST
§4.11's named gap, the prime suspect on OPEN-1 route 1's path. **DDR-1047's unit
boundary is preserved exactly**: spin waits are cycles burned, yield waits are
wall time during which the CPU ran other threads, so the two get **different
line shapes**, never a shared column. `waiters` alone carries both, because a
count is dimensionless.

**Proof, two-sided, on recorded hashes.** M1 (`d36f7d0e0f359824`): the BSP holds
a dedicated lock while a kernel thread on another CPU attempts it; the dump
prints `lock=0xFFFFFFFF8013CF74 hits=0 waitavg=0 waitmax=0 waiters=1`. **M0 —
the same arm on the pre-fix tree** (`1a74d7a0c959b9bb`) — reports **eleven other
locks and that address is absent entirely**. M0 is the load-bearing half:
without it, "the new column prints a number" and "the instrument now sees the
frozen CPU" are indistinguishable. Revert returns `c33afa79f60abdcb` bit-for-bit.

**No gate:** the dump prints only on `[apfreeze]`, which is in
`GLOBAL_FORBIDDEN`, so an assertion on it would be unreachable-passing on every
green run — with some irony, the exact defect this fixes. Do **not** create
`smoke-lockstat` (precedent: DDR-1039, DDR-1005).

**NOT CLAIMED:** no kernel defect is fixed and no cause is named. OPEN-1 and
OPEN-2 are untouched. This changes what a *future* freeze can say, nothing about
any past one. A frozen CPU with **zero** `waiters` anywhere is now a readable
answer — it means the freeze is *not* a lock wait — which the instrument
previously could not produce in either direction.

**§9 — companion process gap, caused by this session.**
`tools/ci/open10_campaign.sh` invokes `make`, so it rebuilds mid-campaign.
Editing `kernel/lock_stat.c` at 22:58:48 during a campaign begun at 22:47:14
rebuilt `kernel.bin` at 22:59:40 **between recorded runs**, with nothing in the
report saying so. **That campaign is VOID and is not reported.** DDR-1023 had
already written the rule — *"hash-verified before AND after every run"* — and
never implemented it in the tool, which is the failure mode itself: a discipline
that lives only in a document. The tool now pins the hash, prints
`kernel_pinned=`, checks before and after every run, and **aborts rather than
warns**.


---

## DDR-1061 — `smoke-sfs-btree-smp4` registered, and the rate campaign stopped as null-on-design

**REGISTERED** on shard 5 (180 s, strict). Gate count 175 → **176**, excluded
7 → **6**. Shard 5 budget 1376 → 1556 s, still under shard 9's 1965 s.

**Registered on the exclusion's OWN stated condition.** `shard_check.sh` has said
since DDR-824: *"Register it when OPEN-10 is fixed."* OPEN-10 is fixed —
**DDR-964** named the mechanism (a create-then-init race making
`cap_ok(CAP_FS_WRITE)` return `-EPERM`, because `sched_create()` made a thread
runnable before its caller minted the capability into `->arg`) and
mutation-checked the fix at eight sites. **The condition asks for a fixed
mechanism, not a bounded rate**, and that distinction is the whole decision.

**The rate campaign cannot answer the question at any reachable N — computed
before spending the hours, not after.** With 0 failures in *n* runs the 95% upper
bound is `1 − 0.05^(1/n)`: n=20 → 13.9%, n=30 → 9.5%, **n=44 → 6.6%**, which
merely *reaches* the 2/30 = 6.7% pre-fix rate `open10_campaign.sh`'s own header
records — no margin, and no power at all to distinguish "fixed" from "still
6.7%". At a cleanly-timed **182 s per run** that is ~2.3 hours of **foreground**
execution, and foreground is the only option (DDR-1060 §10: three background
campaigns each died after 1–5 runs, `setsid` included). **This is DDR-1002's
shape** — null on its own design — with the difference that DDR-1002 found out
after spending the runs. **Do not run this campaign.**

**Measured:** 3 runs, 0 failures, kernel `c33afa79f60abdcb`, hash-verified before
and after each run by DDR-1060 §9's pin. Recorded so its absence is not mistaken
for an untried experiment — **not** as the basis for the decision, since 3/3
bounds the rate below 63% and nothing more.

**Residual risk, stated:** if the defect is not in fact fixed, every suite becomes
measurably likelier to be red, degrading the §NON-NEGOTIABLE 1 criterion the
release depends on. Registered anyway because the exclusion pre-authorises it,
because a gate held out of the matrix is coverage nobody is getting (the
DDR-1046/1049/1060 rot), because **if it reddens that is the measurement** the
campaign provably cannot produce, and because un-registering is one line while
the release is HELD so no promotion is in flight for a red to block.

**NOT CLAIMED:** that the defect is proven gone. **If it reddens:** read the CI
capture, do not re-run the campaign; DDR-964's fix is about *when* `->arg` is
minted, so a recurrence means a `sched_create()` site was missed — un-register
and reopen OPEN-10 with the capture.


---

## DDR-1062 — OPEN-2: the first CI-side rate bound; DDR-1009's 25% refuted

**MEASURED. OPEN-2 remains OPEN — no mechanism named, no fix.** Docs only.

**Why it is possible now.** DDR-1023 established the local route is exhausted and
the evidence is CI-side. What made counting CI greens *mean* anything is
**DDR-1049**: before it, a lone CPU that panicked and died before its banner left
every channel empty, so a run could go green with a panicked CPU in it. From
`32cb8ad` (2026-09-03) `panic_stage=` is set by the **winner** and sits in
`GLOBAL_FORBIDDEN`, so a green suite now genuinely asserts no CPU claimed the
latch. Detectors verified armed; `GLOBAL_FORBIDDEN` reads **76**, matching
§NON-NEGOTIABLE 6's count, so the list is intact rather than silently emptied.

**Measured:** 42 `pradyos-ci` suites at/after that commit, 19 distinct SHAs,
push/PR/dispatch. **Zero `[apfreeze]`, zero `panic_stage=`, zero `gs FAIL`.**

**All 42 count, including the 4 reds** — a red suite still ran the global
forbidden scan, so an `[apfreeze]` would have named itself; counting only the 38
greens understates the evidence.

**Every red attributed, none an AP freeze** (the load-bearing check — a red left
unread could *be* the artefact): `c8c93ed` ×2 `smoke-actiondel` (DDR-1056 splice
class), `b7ff2a3` `smoke-nethammer` (DDR-1055's console splice, since fixed),
`1efbb49` `smoke-surfclose` (splice class). `b7ff2a3`'s capture was **read**:
heartbeats clean to `t=23500`, `ymask` climbing, `rqcpus=2`, `preempt` advancing
— a healthy SMP kernel timing out on a sentinel match, not a frozen CPU.

**Bound:** 0 in 42 ⇒ **95% upper bound 6.9% per suite**.
P(0 in 42 | p=0.25) = **5.7 × 10⁻⁶**, so DDR-1009's figure is refuted;
P(0 in 42 | p=0.10) = 0.012. Per-suite is the right unit — it is what DDR-1009
measured. This supplies the replacement number the "stop quoting ~1 in 4"
warning always lacked.

**NOT CLAIMED:** OPEN-2 is not closed; a rate under 6.9% is still a rate; these
are 42 suites over 19 SHAs, **not 42 binaries**; and DDR-1049's own residual (a
panic faulting before it *wins* the CAS) stays invisible.

**Instrument set now complete for discrimination:** panic loser → `panic_stage=`;
AP timer ISR → `[apfreeze]` + `rip=`/`bt=`; SWAPGS → `gs FAIL`; and DDR-1060's
`waiters=` completes it, the **negative** reading being the valuable half — a
frozen CPU with zero waiters anywhere means the freeze is *not* a lock wait.
Reachability checked, not assumed: the dump is ordered after the `[apfreeze]`
line, and NMI is non-maskable so it lands even on the `cli; hlt` producer.

**DDR-1006 §7's "which loop iteration" — assessed, deliberately NOT built.** It
needs always-on per-iteration counters on exactly the paths where the race lives
— the cost DDR-1047 refused, because an instrument that can *move* OPEN-2 is
worse than none; opt-in is not the escape (DDR-1010/1043: guaranteed OFF in CI,
the only place it appears); and `rip=`/`bt=` already name the site while
`waiters=` answers what it waited on.

**Next: nothing until an artefact appears.** Local route exhausted, CI route
armed and self-diagnosing, rate too low to manufacture occurrences.

---

## DDR-1063 — a live-state table carried a derived quantity, and its input moved without it

**Status:** FIXED (both documents) + GATED (`ci-docstate-check`) + M0/M1/M2/M3.
`kernel.bin` **UNCHANGED**, `c33afa79f60abdcb`, 1,278,346 B — no kernel source is
touched; the gate is a static text check.

**The defect.** `CLAUDE.md` §CURRENT BUILD STATE read *"**1,278,346 B** against
the 1,572,864 B size gate — 396,918 B of headroom"*. But
`1,572,864 − 1,278,346 = 294,518`; the stated figure is `1,572,864 − 1,175,946`,
the headroom of the **pre**-post-quantum kernel. The size was updated as ML-DSA
landed (DDR-1052 → 1054 → 1057 → 1058) and **the subtraction beside it was not**,
so the file **overstated the remaining budget by 102,400 B** — roughly the entire
ML-DSA landing. `docs/PRE_LAUNCH_CHECKLIST.md` §6 carried the same pair one
revision further back, along with a stale gate count (173/7) and DDR free range
(1050+). DDR-1058 had computed 294,518 B correctly in its own text, so the project
held both numbers at once and the wrong one was in the file §MANDATORY FIRST
ACTIONS makes every session read.

**Why no gate saw it.** `Makefile:697` enforces the ceiling against the
**binary**. It says nothing about what a **document** claims, so the doc could
state any number and all 176 gates stay green. And none of `hygiene_check.sh`'s
six checks reads a claim in a document — they cover shards, probe rodata, `_start`
alignment, the resize checker and apt. Not the DDR-1046 shape (a control blind to
its own case); this is the case with **no control at all**.

**Why it matters.** Headroom is what a session uses to decide whether a subsystem
fits *before building it* — DDR-1051, DDR-1058 and DDR-1059 all size candidate
work against it. §INV.18's binding quantity is file **+ BSS** against the 2 MiB
`PT_HI` span, so overrunning it is a **boot** failure found at QEMU time, after
the work is written.

**The fix, and what was deliberately not built.** Rejected: assert the stated size
equals `wc -c build/kernel.bin`. That reddens on every commit changing the kernel
before the docs are updated — i.e. on correct in-progress work — and a check that
reddens on correct work gets `|| true`'d, not obeyed. Shipped instead:
`tools/ci/docstate_check.py` asserts **internal consistency only**,
`size + headroom == ceiling`, with the ceiling **read from `Makefile:697`** so it
cannot drift from the gate it describes. It makes no currency claim.

**Vacuity handled in design, not after.** A regex check over prose that matches
nothing passes forever — the dead-arm class, twelfth-plus instance. So it
**counts its findings and FAILS on zero**, and prints every pairing with
`file:line` so a reader can confirm it looked where they think.

**Proof, four arms, three failure modes.** M1 is not synthetic — it is the literal
pre-fix `CLAUDE.md`, and it fails naming the line and `off by +102400` while the
two *correct* pairings in the same run stay green, so the arm discriminates. M0 on
the fixed tree: `OK - 3 pairing(s) checked, 0 inconsistent`. M2 drifts the wording
past the pattern and the check **must not pass** — without it, "wired up" and
"matches nothing" are the same `rc`. M3 quotes a ceiling the Makefile does not
enforce, and **found a defect in the checker's own first draft**: it printed
*"recompute headroom"* for a ceiling drift, the wrong remedy; the two now report
separately. Measured counts, not predicted — the DDR's draft said 2 pairings and
there are **3**, the extra being §5.1b.1's dated pre-work assessment, which
correctly *passes* because it is self-consistent for its own date.

**Also corrected.** Checklist §5.4: `smoke-sfs-btree-smp4` row removed, 7 → **6
excluded**, and the clause *"waiting on promotion evidence, not on work"*
**retracted** — never part of the exclusion's stated condition, and DDR-1061 §2
shows no reachable N could supply it. Checklist §6 re-measured at `0089e08`: 176
gates, 6 excluded, DDR range 1063+, 1,278,346 B / **294,518 B** with the hash.
§5.1b.1 fact 2 left as written (a dated assessment whose prediction was borne out)
and annotated: the work cost 102,400 B, **26% of the headroom it was measuring**.
`build_status.md` and this file are dated append-only logs and are deliberately
**not** inputs to the check. Hygiene **ALL SIX → ALL SEVEN**; `CLAUDE.md`'s
hygiene item updated to match, that item having drifted from the script before.

**THE THESIS CONFIRMED IN THE SAME SWEEP (DDR-1063 §7b).** Checking
`PRE_LAUNCH_CHECKLIST.md` for the same *class* of defect — an entry not updated
when the work it describes landed — found **three more**, in the document whose
own purpose is "every deferred/open item, one document": **DDR-1062 absent
entirely** (OPEN-2's first CI-side rate bound, the most important new fact about
the top open defect), **DDR-1056 absent entirely** (it *fixed* the
`smoke-actiondel` splice class while the row still read as pending), and **§4.8
claiming the ghost-window repair was "named, not built"** when it is built and
gated — `mouse_inject.sh:131` consumes `PRADYOS_WM_GONE` and refuses the click,
`smoke-ghostclick` gates it, checked in the tree rather than assumed from the DDR
text. **AND FOURTH: §5.3's gate inventory was wrong on four of its seven Group A
claims** -- in the section that opens by saying it was measured by grepping the
Makefile. Re-measured at `c8b041b`: `smoke-smep` and `smoke-smap` EXIST (DDR-1040/1041,
the latter not even listed), `smoke-mce` EXISTS under a name the row got wrong
(`smoke-mc` never existed), `smoke-wx` never existed either (the real gate is
`smoke-wxkernel` -- the same wrong name DDR-1040 found in CLAUDE.md), two whole
gates were absent (`smoke-shake`, `smoke-mldsa`), and Group D listed `smoke-poll`
as MISSING three paragraphs after saying it EXISTS. All four corrected. **Five
stale items in one sweep of one document, and the new check catches one shape of
one of them** — recorded so the residual's
size is visible rather than implied.

**THE WHOLE SURFACE THEN MEASURED (DDR-1063 §7c), 2026-09-06.** Every
`` `smoke-*` `` name in `CLAUDE.md` (**116** distinct) against every `^smoke-*:`
target in the Makefile (**182**). **59 named names have no target, and that is NOT
a defect count** — most are legitimately-unbuilt backlog rows, which is a planning
table doing its job. That distinction is itself the finding, and it **confirms**
§7b's reason for deferring the mechanical gate-inventory checker rather than
refuting it: the check needs a machine-readable way to separate *"claimed to
exist"* from *"named as future work"*, and grepping cannot supply it. What the
sweep does establish is a **class**: **five gate names that have never existed
while the real gate did** — `smoke-wx`→`smoke-wxkernel` (DDR-1040),
`smoke-mc`→`smoke-mce` (§7b), `smoke-lazystack`→`smoke-stack-demand` (the Group A
row itself), `smoke-vdso-read`→`smoke-vdso` (DDR-1005), and **new here**
`smoke-maximize`→**`smoke-wmmax`** — and **DDR-1067 later found a sixth**,
`smoke-pipes`→`smoke-shell`, so the count is stated with its date rather than as
a total. The fifth is the §7b shape exactly: CLAUDE.md's
Group E row read *"DDR-719 caps at 512×512; lift to real geometry"* as if unbuilt
and checklist §5.3 said maximize *"shipped as DDR-1007 under a different gate
name"* **without naming it**, while `smoke-wmmax`'s Makefile header reads
*"Layer-7 maximize gate (DDR-719, retargeted by DDR-1007)"* and it asserts
**`w > 512`**, with the failure message *"DDR-1007 did not take effect"* — the very
cap the row described is what the gate proves is gone — and it further asserts the
client HONOURED the size, so a compositor publishing a geometry nothing acts on
fails. **Verified in the Makefile, not inferred from the DDR.** **Why it is not a
typo:** a wrong gate name reads as *unbuilt work*, so the cost is building
something twice, or "fixing" something already fixed; five instances is a class,
and the cause is structural — **the name is written when the work is planned and
the gate is named when the work lands, and nothing has ever reconciled the two**.
Both the CLAUDE.md Group E row and checklist §5.3 corrected. **Document sweep
total: six stale items**, of which `ci-docstate-check` catches one shape of one.

**NOT CLAIMED:** the documented numbers are not thereby *correct* — a stale but
self-consistent pair still passes, which is exactly what checklist §6 was;
currency remains §NON-NEGOTIABLE 11's human discipline. No kernel defect is fixed
and no open issue moves. The check covers **one** derived quantity; gate counts,
NSI max and the DDR free range are equally derivable and are not checked, because
each needs a different oracle. And the 102,400 B error's downstream effect was not
measured — DDR-1058 used the right figure, so the claim is that a wrong number sat
in the trusted file, not that it caused harm.

---

## DDR-1064 — the rq-3 discriminator had the same race it was built to settle

**Status:** FINDING recorded + DDR-1030 CORRECTED (it contradicted itself) +
instrument FIXED + M1 forced-proof. **No scheduler defect is named or fixed.**
`kernel.bin` `c33afa79f60abdcb` → **`d19cd33755330510`**, still 1,278,346 B.

**Trigger.** CI 34003737145, shard 4, `smoke-smppreempt`, tip `e9ed2c9`:
`[smp] resched FAIL ipis=0 ran=1 idle=1 idle2=1` — **the first real `idle2=`
reading ever produced**; every prior appearance in the repo is DDR-1030's design
text or its forced mutant. `e9ed2c9` changes three `.md` files and nothing else,
and the shard's own post-gate assertion printed `kernel.bin: OK`, so the binary
is bit-identical to the green tip and the failure cannot be that commit's.
`ran=1` says **the property under test HELD**.

**DDR-1030 contradicted itself.** §5's table: `idle=1 idle2=1` = "a scheduler
defect". §6, four paragraphs later: "`idle2=1` on a healthy boot is the expected
reading, which is what makes `idle2=0` informative." §6 generalised from its own
forced mutant, which ran with `ipis=1` — a *delivered* kick — whereas the FAIL
branch's precondition is `ipis=0`.

**And §5 is wrong too — the finding.** The kernel kicks from **inside**
`sched_unblock` (`sched.c:1837`); the proof samples `idle_after` **after that
call returns** (`main.c:1031`), and `o->idle` is live. So a CPU can **enter**
idle between the kernel's loop and the sample: no kick was owed when the kernel
looked, and a correct system still prints `idle2=1`. DDR-1030 closed DDR-1004's
window (a CPU *leaves* idle before the call) and **opened its mirror image**.
The instrument has the same class of race it was built to settle — the
DDR-1046 / DDR-1060 shape a third time, a control that cannot see the case it
exists for. **This capture is consistent with a correct kernel.**

**The wrong reading had been copied into three documents** (`BUILD_TRACKER.md`,
`PRE_LAUNCH_CHECKLIST.md` ×2). All corrected. A wrong reading in three places is
how the next session convicts the scheduler on this capture with
§NON-NEGOTIABLE 3 *satisfied on paper* and the mechanism never named.

**Fix — ask the kernel, do not paraphrase it.** DDR-1014 already wrote the rule
("the two loops must ask the same question or the proof is testing a paraphrase")
and applied it to the **predicate**; the **instant** still did not match, and the
proof cannot fix that from outside because every sample it takes is at the wrong
time by construction. `sched_unblock` now records what its own loop saw, at the
only instant that matters: `dbg_ub_saw_idle` (an idle non-self CPU was visible)
and `dbg_ub_kicked` (`smp_resched_one` actually delivered). **On the TCB, not in
a global** — `sched_unblock` runs from MSI-X interrupt context on the virtio-blk
completion path (DDR-1014), so another CPU's unblock would clobber a global
between the proof's call and its read. **§NON-NEGOTIABLE 10:** both fields get
explicit initialisers in `sched_create`. **Cost:** two plain stores in the
CAS-succeeded slow path, never the fast path, no `rdtsc` — deliberately not the
cost DDR-1047 refused, which could have *moved* OPEN-2.

`saw_idle` mirrors the **kernel's** predicate, not the proof's: the proof carries
`!o->is_bsp` because `smp_resched_one` declines the BSP, and paraphrasing that
here would reintroduce the drift DDR-1014 removed. A BSP-only-idle boot therefore
reads `kidle=1 kkick=0` and is **correct**, which the proof's predicate cannot
express at all.

**Reading the line from now on:** `kidle=1 kkick=0` is the **only** reading that
convicts; `kidle=0` exonerates whatever `idle=`/`idle2=` say; `kidle=1 kkick=1`
with `ipis=0` would mean the counter, not the kick, is the defect.

**M1 forced-proof** (`else if (0 && …)`, DDR-1030's own construction, kernel
`4786243a1f71a021`): `[smp] resched FAIL ipis=1 ran=1 idle=1 idle2=1 kidle=1
kkick=1` — the fields are wired, and on a healthy boot they agree with reality
and say a kick *was* delivered. Reverting returns `d19cd33755330510`
**bit-for-bit**.

**The verdict is deliberately unchanged.** `resched FAIL` keeps its meaning and
stays in `GLOBAL_FORBIDDEN`. Collapsing this to SKIP would green the gate and
delete the coverage DDR-1014 built — the trade DDR-1012 and DDR-973 each had to
undo, and DDR-1030 §3 refused once already.

**NOT CLAIMED:** no scheduler defect is named or fixed; OPEN-1/2/12/13 untouched.
The gate is not made deterministic — the FAIL can still fire on a correct system
until the non-racy field replaces the racy one in the *verdict*, which this does
not do, because changing a verdict on one capture is how coverage gets deleted.
No rate is measured. And the new fields are proven **wired**, **not** proven on a
genuinely failing path — the same limit DDR-1060 recorded about DDR-1047's M1,
because no genuine missed kick can be manufactured locally.

---

## DDR-1065 — `ptnode_in_use` underflows on every COW fork: artefact, then fix

**Status:** GATE BUILT (`smoke-sharedpte`, shard 4, strict — the DDR-1003 §5.1
design, unbuilt since that DDR) + FIXED (DDR-1003 §5.2's narrow version) + M1 =
the literal pre-fix tree. `kernel.bin` `d19cd33755330510` → **`a9d8bc933595ec0d`**,
1,278,346 → **1,282,442 B** (headroom recomputed to **290,422 B** in the same edit,
per DDR-1063). Gates **176 → 177**.

**Why now, when DDR-1003 deliberately did not.** It gave two reasons. The first —
*"no gate observes this today, so there is no failing artefact, and
§NON-NEGOTIABLE 3 forbids the fix"* — is **addressed by building the gate**, which
is why gate and fix land together. The second — *"days before a release"* — has
**expired**: the release is held indefinitely pending OPEN-1/2/12/13 with no
promotion in flight, the same circumstance DDR-1061 used. Saying so beats quietly
acting against a recorded decision. DDR-1003 also named the cost of leaving it:
*"a loaded gun: the next person to add a leak gate spanning a fork will get a
wrapped counter and a mystery."*

**Mechanism, verified in the tree rather than taken from DDR-1003** (this session
has repeatedly found DDR text that no longer matched): `ptnode_alloc` increments
once per frame; `vmm_cow.c:101` `pmm_incref` — the kernel's **only** incref site,
measured — raises the refcount with no second increment (correct, no new frame);
`pmm.c:192` returns **without freeing** when refcount > 1; `ptnode_free`
decremented **unconditionally**; and `vmm.c:371` skips only `PTE_SW_SHARED`
(0x400) while a COW page carries `PTE_SW_COW` (0x200) — different bits — so the
leaf is freed from **both** address spaces. **One `++`, two `--`, one release.**

**The gate's design is the point.** DDR-1003 §5.1 warns the ordinary leak shape
(fork, child **writes**, both exit) is balanced and would **pass** — a gate built
the obvious way tests nothing, the dead-arm class again. So the probe's child
**exits without writing**, and it uses the **real** fork path
(`vmm_fork_address_space_cow`), differing from `cow_selftest` by exactly one line.
Deterministic and kernel-side — no ring-3, no reap poll, no timing — so it is not
an intermittent gate and its stated N is **1**.

**MEASURED, two-sided:** pre-fix `e256aa4802882aa6` prints
`SHAREDPTE before=0 after=18446744073709551615` — `0xFFFFFFFFFFFFFFFF`, **−1,
wrapped from ONE fork**; fixed `a9d8bc933595ec0d` prints `before=0 after=0
PRADYOS_SHAREDPTE_OK`. In the same capture the two pre-existing readers are
unaffected (`kheap stress base=0x0 after=0x0`, `vmm unmap reclaim 0x0 -> 0x0`),
re-verifying DDR-1003 §5's claim rather than assuming it.

**THIS DDR'S OWN DRAFT WAS WRONG AND THE MEASUREMENT CORRECTED IT.** §5 first read
*"the wrap itself is not demonstrated … needs ~2^64 forks."* It needs **one**,
because `kheap_outstanding()` is legitimately 0 at that point in boot. Recorded as
a correction rather than silently rewritten: I under-stated the defect, and
reading the actual output is what caught it.

**Fix:** `pmm_free_pages` returns 1 = released / 0 = reference dropped;
`pmm_free_page` forwards; `ptnode_free` decrements only on a real release.
`void`→`int` is source-compatible with no `warn_unused_result`, so all **132**
existing call sites stand unchanged. DDR-1003 §5.2's **wrong** fix is named and
refused: having `vmm_cow_fault` call `ptnode_free` would decrement where nothing
was released and merely move the imbalance.

**`GLOBAL_FORBIDDEN` deliberately NOT touched.** Both directions are covered on
the gate's own shard (`EXTRA_SENTINEL` for the OK marker, `FORBIDDEN_SENTINEL` for
the FAIL string). The precedents for a global entry — DDR-981's `[apfreeze]`,
DDR-1049's `panic_stage=` — are **intermittent** defects that could hide in a green
run; this one is deterministic and cannot, and §NON-NEGOTIABLE 6 documents how
fragile that list's terminator is.

**NOT CLAIMED:** no leak is fixed and none existed — frames were always released
correctly, the **counter** was wrong; this is an accounting fix. The two existing
`kheap_outstanding()` readers were never wrong. No open issue moves — OPEN-1,
OPEN-2, OPEN-12, OPEN-13 untouched.

---

## DDR-1066 — the AETHER agent never executed anything, and the gate's execute arm was the string it printed

**Status:** FINDING + FIXED + gated + M1/M2 on distinct hashes.
`kernel.bin` `a9d8bc933595ec0d` → **`dde6c5d10748842d`**, 1,282,442 → 1,286,538 B
(headroom 286,326 B). **No kernel defect is fixed and no open issue moves.**

**The finding.** `smoke-aether`'s own Makefile comment states the claim — *"the
agent submits `ACTION_WRITE_FILE` which sovereign mode auto-approves; **the agent
executes it** and exits. End-to-end: queue -> daemon -> agent -> approve ->
**execute** -> done."* The agent does not execute it. `user/agent_base.c:182` on
`AE_APPROVED` did:

```c
    printf("AETHER_AGENT_EXEC WRITE_FILE %s\n%s\n", path, data);
```

and nothing else. `grep -n "SYS_OPEN\|SYS_WRITE\|SYS_CREAT" user/agent_base.c`
returns **nothing**, in **either** branch, and `AETHER_TEST_MODE` defaults to 1 so
the CI-exercised branch is the one that printed. **`data` is
`"PRADYOS_AGENT_VERIFIED"` — one of the gate's four required sentinels** — so the
end-to-end gate's execute arm asserted on a `.rodata` literal and **could not fail
for the reason it exists**. The dead-arm class, thirteenth-plus instance and the
**first found in the product** rather than in a gate or an instrument. The
baseline capture reads `AETHER_AGENT_EXEC WRITE_FILE /tmp/aether_test.txt` then
`PRADYOS_AGENT_VERIFIED` — which a human reads as a completed write.

**Not blocked; never wired.** Checked in the tree, not assumed: `kernel/exec/elf.c:320-321`
gives every `elf_load`ed process `CAP_FS_READ | CAP_FS_WRITE` and
`root_mnt = vfs_default_mnt()` (the FAT volume), and `fat32_write` exists
(`fat32.c:529`, wired at `:732`). **And the path would have failed anyway:** the
FAT volume's only directory is `::/DOCS` — one `mmd` in the whole Makefile — so
there is **no `/tmp`**, and a correct implementation of that exact path would have
returned `-ENOENT`. That is the trap waiting for anyone who "just adds the write".

**The discipline was already written down, in this repo, for this hazard.**
`user/actionreadtest.c:101` says *"3. EXECUTE, and only now"* and *"4. VERIFY THE
BYTES. This is what makes the gate non-vacuous: a probe that skipped step 3 still
prints an OK line, but cannot print the content."* The 3C probes follow it; the
one real agent did not — and DDR-1022 established `agent_base.c` is the **only**
agent program, so "AETHER executes approved actions" rested entirely on this file.
DDR-1013's scope correction **stands**: the kernel approves, the agent acts, and
no kernel executor is added.

**Fix.** Open/write/close, then **reopen without `O_CREAT`** and read back, and
print the marker **from the read-back buffer**, never from `data`. The missing
`O_CREAT` is load-bearing: `vfs_open` on an absent file returns `-ENOENT`, so a
build that skipped the write cannot reach the print. **The existing sentinel
becomes live with no edit to that arm** — the cleanest available proof it was
dead.

**M1 and M2 are the whole argument, and they do the same filesystem work: none.**
M1 (no write, print from the buffer, `248afcf994645ab5`) **FAILS** rc=2 with
`AETHER_AGENT_EXEC_FAIL step=open_r rc=-2` and *"required pattern
'PRADYOS_AGENT_VERIFIED' not found"*. M2 (no write, print the **literal** — the
pre-fix behaviour, `46aaf0304f395b6f`) **PASSES** rc=0. The only difference is
where the printed bytes come from, and that alone decides whether the gate can
see it. The baseline is M2's independent confirmation, being the pre-fix tree
itself, green. Reverting returns `dde6c5d10748842d` **bit-for-bit**.

**M2 CORRECTED A DESIGN CLAIM OF THIS DDR'S OWN.** §3.2 introduced
`PRADYOS_AGENT_EXEC_OK … n= first= last=` as if it added discriminating power —
**M2 prints it too, with correct values**, because an agent holding the data can
compute all three without touching a filesystem. What convicts is the `-ENOENT`
the agent cannot manufacture. `n=` is kept for the narrower claim it does cover:
a **real** write that stores the wrong length. Also: `-Werror
-Wunused-function` caught M2 dropping the error path, so a future change deleting
every failure path from this executor **will not compile**.

**Gate.** `smoke-aether` gains the **exact** pattern `PRADYOS_AGENT_EXEC_OK
path=/AETHER.TXT n=22 first=P last=D` (DDR-1044's exact-value discipline) and
`AETHER_AGENT_EXEC_FAIL` as a `FORBIDDEN_SENTINEL`, so a failed execution names
itself rather than being detectable only as an absence. **No new gate** — the
point is that the gate which already claimed this can now fail for that reason.

**NOT CLAIMED:** `ACTION_SEND_IPC` is **still** unwired — checklist §4.1 is
corrected, not closed; it was right about SEND_IPC and far too narrow about the
cause. No kernel defect is fixed: the policy engine, the capability check and the
FAT32 writer were all correct and complete. No open issue moves. The live
(Ollama) branch is unchanged and unexercised. And the agent executes the **one**
action type it submits — dispatching the template on action type is a larger
change and is not attempted.

### DDR-1066 §8.4 NARROWED 2026-09-06 — the changed code did not execute in the failing boot

The `smoke-blk-integrity` red recorded above was left **not attributed and not
exonerated**. A cheap measurement narrows the second half of that, and it is
worth the note because the first attempt at it was **vacuous**: scanning the
regression's `build/gatelogs/reg*.log` files found almost nothing, because those
are **make output, not serial captures** — the exact methodology defect DDR-1023
§recorded, walked into again, and caught only by reading the counts instead of
reporting them.

Against the **serial** captures the answer is clean. The red capture contains
**zero** `PRADYOS_AGENT_START`; the passing capture of the same gate on the same
binary reaches it at **line 413 of 466**:

```
PASS  413: PRADYOS_AGENT_START task=test mode=test
      414: PRADYOS_AGENT_EXEC_OK path=/AETHER.TXT n=22 first=P last=D
      417: PRADYOS_AGENT_DONE
RED   (351 lines, no AGENT_START anywhere)
```

DDR-1066's diff is **entirely inside the post-`AE_APPROVED` path**, reached only
after that first line prints, so the boot stopped **before any of it could run**.
**Still not fully exonerated:** `agent_base.elf` is embedded in `kernel.bin`, so a
layout or size effect is not excluded — only a behavioural one.

**Two further facts from the same sweep, since they answer a question worth
asking:** across every serial capture on disk, `AETHER_AGENT_EXEC_FAIL` appears
**zero** times, and `smoke-aether-sfsroot` — the gate whose SFS root could have
made the new `/AETHER.TXT` write fail — **does not boot the agent template at
all** (its capture has the AETHER self-test's own sentinels and no
`PRADYOS_AGENT_START`). The executor is also confirmed working outside its own
gate: `blkint-pass.log` carries `PRADYOS_AGENT_EXEC_OK`.

---

## DDR-1067 — PRISM had no quoting at all, and the obvious gate arm for it is vacuous

**Status:** IMPLEMENTED + gated on `smoke-shell` + M1 (the pre-fix tokenizer,
verbatim). `kernel.bin` `dde6c5d10748842d` → **`cc8135a9463eefed`**,
**1,286,538 B unchanged**. **No kernel change** — `tokenize()` is ring-3 shell
code. Gate count stays **177**.

**The row was stale before the work started.** Group D's *"PRISM pipes /
redirection / quoting / job control / scripting"* named four things, and
`smoke-shell`'s own PASS line — read, not assumed — already says *"redirect(> >>
< 2>) + truncate/append + stderr + pipes(N-stage, >4KiB)"*. **Pipes and
redirection were shipped and gated**, and `smoke-pipes`, the gate name that row
carried, does not exist — the DDR-1063 §7c class again, sixth instance.

**The defect.** `prism.c:176` split on runs of spaces and nothing else. So `echo
"hello world"` passed **three** arguments with the quote characters still in
them, and **a filename containing a space could not be named at all** — `touch
"my file"` created two files, neither of them the one asked for. Newly reachable
beyond the shell: DDR-1032b wired PRISM's `run` through to `execve`'s argv
marshalling, so a quoting defect now propagates into the **child process**.

**THE OBVIOUS GATE ARM IS VACUOUS, AND THAT WAS MEASURED BEFORE IT WAS WRITTEN**
— third time this class has been caught in design text before any code
(DDR-1039 §3.1, DDR-1058). The natural test is `echo "one two"` asserting `one
two`; `prism.c:591` **joins argv with single spaces**, so quoted and unquoted
print **byte-identical output** and the arm passes on a shell with no quoting
whatsoever. The two arms that discriminate:

- `run /ARGTEST.ELF "gamma delta"` → **`PRADYOS_ARGC=2`**, where the pre-existing
  DDR-1032b arm four lines earlier runs the same probe unquoted and prints
  `ARGC=3`, so the two counts cannot be confused in one log; plus
  `PRADYOS_ARGV=gamma delta`, **one argv entry containing a space**, which no
  unquoted line can produce. Both directions, per DDR-1039.
- `echo "q  9k2"` with **two** internal spaces — the one thing the tokenizer
  destroys and `echo`'s join cannot restore. Vacuity **checked**: `grep -c`
  returns 1 on the passing capture, and the only other `9k2` line has no double
  space.

**Fix.** `'...'` and `"..."` both literal, stripped in place through a write
cursor trailing the read cursor, so the result is never longer than the input and
every caller is unchanged. **An unterminated quote returns -1 and the line does
not run** — a typo must not execute a command the user did not write, and this
shell has no continuation prompt to offer instead.

**M1 is the pre-DDR-1067 tokenizer restored verbatim** (`2f89f3829acb888d`), not
a synthetic defect, and its log carries all three halves: `PRADYOS_ARGC=3`
**twice**; `prism> "q 9k2"` — one line showing that the quotes survived into the
argument *and* that the space run was collapsed; and `prism> unterminated"`,
silently accepted. Reverting returns `cc8135a9463eefed` bit-for-bit.

**THIS DDR'S OWN FIRST DRAFT WAS WRONG AND CHECKING IT CAUGHT IT.** §4.1 claimed
a quoted `">"` would become a literal argument, *"which is the correct shell
behaviour"*. **False:** `|`, `>`, `>>`, `<` and `2>` are matched by `strcmp` on
the token **after** the quotes are stripped (`prism.c:385`, `:469`), so `echo
">"` still redirects. Making it literal needs a *was-quoted* flag threaded
through two loops. **Recorded as a limitation rather than shipped as an
unverified claim.**

**NOT CLAIMED:** no new gate — the arms belong on `smoke-shell` where PRISM's
line handling already runs, the same reasoning DDR-1039 recorded for refusing
`smoke-readline`. No backslash escapes. No expansion inside double quotes (`"$?"`
behaves as `$?` does, the substitution being a whole-token suffix match applied
*after* tokenizing). **Job control and scripting remain on the Group D row**,
which is corrected rather than closed.
