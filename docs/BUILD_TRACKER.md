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
| **OPEN-1** | `smoke-surfdestroy` intermittently misses its sentinel | unknown | open, passive |
| ~~OPEN-11~~ **CLOSED** | `smoke-sha256` (and `smoke-rqstress-liveness`) failed after the first run on a fresh image | **ROOT-CAUSED AND FIXED — DDR-831.** `blk_selftest` wrote its scratch sector at a hardcoded LBA 1500, chosen when the kernel was capped at 512 sectors; DDR-827 grew the kernel to ~1666 sectors, so the self-test wrote *into the kernel image*, and QEMU persisted it — corrupting the probe ELFs in `.rodata` for every later boot | **CLOSED — CONFIRMED BY CI.** Run 30944847959 on `93ceee7`: all 11 jobs green, with `PASS smoke-sha256 (90s)` and `PASS smoke-rqstress-liveness (180s)` actually executed. The build guard printed `kernel ends at LBA 1666, scratch sector 4095 — clear` — 1666 > the old literal 1500, direct confirmation of the overlap. Locally 20/20 + 5/5. |
| **OPEN-2** | historical intermittent CI reds | partly OPEN-1/10. **DDR-828 removed the largest contributor**: 7 of 8 reds on 2026-08-03 were a stale 60 s window on `smoke-syscallfuzz`, not a defect. | open — `smoke-resched` and `smoke-blkmq-trace` each have ONE occurrence, triaged not fixed |
| **OPEN-9** | `smoke-shell` fails locally, passes CI, identical binary | **leaked QEMU holding the image write-lock** | **misattribution FIXED (DDR-823)**; root cause not yet caught on a `smoke-shell` failure |
| **OPEN-10** | `'btree churn FAIL'` during unrelated `-smp 4` gates | see §5 — likely a manifestation of B#3 | **open, gates promotion** |
| **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall | timer/IRQ delivery under SMP | open — last 🔴 in Phase 3 |
| **smoke-x25519** | ~~probe does not reach its sentinel~~ | was host contamination + a stale artefact (DDR-822/823) | **RESOLVED — 4/4 clean-host passes, now in shard 3.** Unblocks Ed25519 → ACC → AGS once green in `main` |
| OPEN-7 | per-boot probe selection | — | CLOSED (DDR-804) |
| OPEN-8 | console input loss | — | CLOSED (DDR-809) |

### The recurring structural defect — FIFTEEN instances

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

**Rule earned: when a check discards input rather than rejecting it, the discard
must be loud.** And: **a tracker line is a label FOR a requirement, not the
requirement — build from the spec text.** Instances 11 and 12 extend it — the invalid case an optimiser
will find is the one that scores well, so it needs an explicit branch and its
own rejection message, never a fall-through comparison.

---

## 5. OPEN-10 — current diagnosis

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
| 8 | B#3 `-smp 4` virtio-blk stall | ⬜ **do before more OPEN-10 work** |

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

### Section 3D — ring-3 / daemon #45–65 (TASK 12) — 8 of 21 done

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

9 goals.md · 10 subconscious loop · 11 MOSS pipeline · 12 OCR→memory ·
13 multi-modal context · 14 privacy mode (ring-3) · 15 model routing ·
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
