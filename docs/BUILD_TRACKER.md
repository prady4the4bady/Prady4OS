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
| 10 | Quantum Layer | 4 | 0 | 0 | 4 | FUTURE |
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
