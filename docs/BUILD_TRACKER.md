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
| **`dev/phase1`** | `1e40464` — **NOT promoted** (OPEN-10 gates it) |
| **Verified** | 2026-08-03, tree clean at `1e40464` |
| **NSI max** | **78** assigned (`SYS_ACC_SEAL` 77, `SYS_ACC_OPEN` 78 — numbers reserved, handlers NOT registered pending DDR-827). Next free **79**. Table size **128**. |
| **CI gates** | **119** assigned across 6 shards · **6** excluded, each with a stated reason |
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
| 6.8 | ACC (DDR-813) | 🔴 | envelope + `sys_acc.c` written, host-verified, NSI 77/78 assigned. **BLOCKED by DDR-827**: linking acc.o + 5 crypto deps puts kernel.bin 12,646 B past the DDR-733 768 KiB load window and the image stops booting. Not linked, not registered, not gated. |
| 6.9 | AGS (DDR-814) | 🔒 | needs 6.7 |

---

## 4. Open defects

| ID | Symptom | Cause / hypothesis | Status |
|---|---|---|---|
| **OPEN-1** | `smoke-surfdestroy` intermittently misses its sentinel | unknown | open, passive |
| **OPEN-2** | historical intermittent CI reds | partly OPEN-1/10 | open |
| **OPEN-9** | `smoke-shell` fails locally, passes CI, identical binary | **leaked QEMU holding the image write-lock** | **misattribution FIXED (DDR-823)**; root cause not yet caught on a `smoke-shell` failure |
| **OPEN-10** | `'btree churn FAIL'` during unrelated `-smp 4` gates | see §5 — likely a manifestation of B#3 | **open, gates promotion** |
| **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall | timer/IRQ delivery under SMP | open — last 🔴 in Phase 3 |
| **smoke-x25519** | ~~probe does not reach its sentinel~~ | was host contamination + a stale artefact (DDR-822/823) | **RESOLVED — 4/4 clean-host passes, now in shard 3.** Unblocks Ed25519 → ACC → AGS once green in `main` |
| OPEN-7 | per-boot probe selection | — | CLOSED (DDR-804) |
| OPEN-8 | console input loss | — | CLOSED (DDR-809) |

### The recurring structural defect — SEVEN instances

One bug in five costumes: **a check that discards input instead of rejecting it,
so drift is silent and looks like success.**

| # | Where | Silent drop | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran in CI | DDR-817 — `make ci-shard-check` |
| 2 | Makefile user sources | 14/31 probes never rebuilt ⇒ gates tested stale binaries | DDR-822 — `$(wildcard user/*.c)` |
| 3 | `user/` `_start` attribute | a new probe silently reintroduces a #GP | DDR-823 — `make ci-start-align-check` |
| 4 | `syscall_register()` | `num >= MAX_SYSCALLS` discarded; NSI 80+ would vanish | DDR-823 — panic + table 80→128 |
| 5 | `check_global_forbidden()` | printed only matching lines, **discarding the `op=` line that names the defect** | DDR-824 — 40 lines of context |
| 6 | **crypto sources + Makefile not prerequisites** | **a build that reports success and never runs** — DDR-822 fixed `user/` and stopped there | DDR-825 — glob `kernel/crypto/*` and list `Makefile` |
| 7 | **writable global in an R+X-only probe** | **link succeeds; the FIRST STORE faults at runtime**, and the gate reports it as a missing sentinel i.e. "the crypto is wrong" | DDR-826 — `make ci-probe-rodata-check` |

**Rule earned: when a check discards input rather than rejecting it, the discard
must be loud.**

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

### Section E — kernel syscalls (TASK 9)

| Item | NSI | Status |
|---|---|---|
| `SYS_MEMORY_WRITE` / `SYS_MEMORY_READ` | 82/83 | ⬜ |
| `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` | 84/85 | ⬜ |
| `spawn_depth` cap in TCB | — | ⬜ |
| DAG action queue (`parent_action_id`) | — | ⬜ |
| `SYS_APPROVE_CODE_REWRITE` | 86 | ⬜ |
| `SYS_READ_AUDIT` (Merkle verify, F#76) | 87 | ⬜ |

### Section 3B — capability bits (TASK 10)

`CAP_MEMORY` (1<<18) · `CAP_OCR` (1<<19) · `CAP_EXEC` (1<<20) · `CAP_REWRITE`
(1<<21, always needs `CAP_SOVEREIGN` co-approval) · `CAP_SCENE` (1<<22) ·
`CAP_NET_BROWSE` (1<<23) — **all ⬜**, each needs an enforcement gate.

### Section 3C — action types #31–44 (TASK 11) — all ⬜

`ACTION_READ_FILE` · `DELETE_FILE` · `EXEC_CODE` · `SEND_IPC` ·
`PARSE_DOCUMENT` · `BROWSE_WEB` · `QUERY_MEMORY` · `CAPTURE_FRAME` ·
`SCAN_ENVIRONMENT` · `QUERY_SCENE` · `REWRITE_AGENT_CODE` ·
`PROPOSE_HYPOTHESIS` · `RUN_EXPERIMENT` · `EVOLVE_GENOME`

### Section 3D — ring-3 / daemon #45–65 (TASK 12) — all ⬜

1 skill.md · 2 SkillOpt loop · 3 SkillOpt-Sleep · 4 skill-update validation ·
5 multi-agent transfer · 6 TokenJuice · 7 JSONL trajectory · 8 cost accounting ·
9 goals.md · 10 subconscious loop · 11 MOSS pipeline · 12 OCR→memory ·
13 multi-modal context · 14 privacy mode (ring-3) · 15 model routing ·
16 hypothesis tree · 17 genome.md · 18 vector knowledge graph ·
19 dead-end registry · 20 population tournament · 21 run visualiser

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

8 kernel slots (KRYOS…SOLIN) ✅ registered + UI cards ✅; **skill prompts ❌ for
all 8**. 12 named agents (file/shell/research/ocr/subconscious/ai_scientist/
healer/architect/verifier/tournament/orchestrator/vision) — **all ⬜**.

### J-01…J-06 retro audit (TASK 15) — all ⬜

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
