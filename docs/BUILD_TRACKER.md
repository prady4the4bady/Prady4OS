# PRADYOS — BUILD TRACKER

**Live dashboard source of truth. Update in the same commit as any merged
feature. If this drifts from the tree, it is a defect — see the four instances
of exactly that failure recorded below.**

---

## 1. Last verified state

| | |
|---|---|
| **`main`** | `b823bb5` — DDR-819 ChaCha20-Poly1305, two greens |
| **`dev/phase1`** | `0c31d11` — **NOT promoted** |
| **Verified** | 2026-08-03 |
| **NSI max** | **76** (`SYS_METRIC_READ`). Next free: **77**. Table size now 128. |
| **CI gates** | 117 assigned across 6 shards · 5 excluded with reasons |
| **CI wall-clock** | ~25 min (was 2 h 08 m before DDR-817) |

**Stale feature branches — do not delete, do not merge.** `feature/arm64`,
`feature/arm64-grace`, `feature/apple`, `feature/rv64` all sit at `a398b62`.
They are landing zones for Phase 0.2/0.3, not live work.

---

## 2. Phase summary

Carried forward from the Aug 2 assessment. **Not re-verified line-by-line this
session** — stated so rather than implying a fresh audit.

| Phase | Name | Items | ✅ | ⚠️ | ❌ | % |
|---|---|---|---|---|---|---|
| 0 | Toolchain & Build | 11 | 4 | 4 | 3 | 36% |
| 1 | Bootloader | 9 | 6 | 2 | 1 | 67% |
| 2 | NEXUS Kernel Core | 57 | 46 | 5 | 4 | 81% |
| 3 | Driver Framework | 33 | 24 | 1 | 5 | 73% |
| 4 | Filesystem Layer | 25 | 22 | 0 | 0 | 88% |
| 5 | Userspace Foundation | 40 | 31 | 2 | 2 | 78% |
| 6 | Crypto Stack | 9 | 4 | 1 | 2 | 44% |
| 7 | AETHER Agent Runtime | 50 | 41 | 8 | 1 | 82% |
| 8 | Sovereign Desktop | 30 | 27 | 1 | 2 | 90% |
| 9 | Assembly Optimization | 7 | 1 | 2 | 4 | 14% |
| 10 | Quantum Layer | 4 | 0 | 0 | 4 | FUTURE |
| **Total** | | **286** | **213** | **27** | **31** | **74%** |

---

## 3. Open defects

| ID | Symptom | Hypothesis / cause | Status |
|---|---|---|---|
| **OPEN-1** | `smoke-surfdestroy` intermittently misses `PRADYOS_SURFDESTROY_CHURN_OK` | unknown | open, passive |
| **OPEN-2** | intermittent CI reds, ~50% historical | partly explained by OPEN-1/10 | open |
| **OPEN-9** | `smoke-shell` fails locally, passes CI, identical binary | **leaked QEMU holding the image write-lock** — explains reproduces-then-clears, and why reverts never helped | **misattribution FIXED (DDR-823); root cause not yet confirmed on a `smoke-shell` failure** |
| **OPEN-10** | `GLOBAL_FORBIDDEN` hit `'btree churn FAIL'` during unrelated gates | SFS B+tree churn probe racing under `QEMU_SMP=4` | **open — gates promotion.** Seen twice: `smoke-smp` (shard 5) and `smoke-rqstress` (shard 3), consecutive runs, both `-smp 4`. Rerun of the first was green ⇒ intermittent, not a flake |
| **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall | timer-stall hypothesis; DDR-777 probe shipped | open — last 🔴 in Phase 3 |
| **smoke-x25519** | probe does not reach its sentinel in QEMU | unknown; one PASS observed but under contaminated host conditions ⇒ **carries no weight** | excluded from shard matrix; **blocks DDR-813** |
| OPEN-7 | per-boot probe selection | — | CLOSED (DDR-804) |
| OPEN-8 | console input loss | — | CLOSED (DDR-809) |

### The recurring structural defect — four instances, two sessions

One bug wearing four costumes: **a check that discards input instead of
rejecting it, so drift is silent and looks like success.**

| # | Where | Silent drop | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran in CI, incl. every crypto primitive's | DDR-817 — `make ci-shard-check` |
| 2 | Makefile user sources | 14 of 31 probes never triggered a rebuild ⇒ gates tested stale binaries | DDR-822 — `$(wildcard user/*.c)` |
| 3 | `user/` `_start` attribute | a new probe silently reintroduces a #GP | DDR-823 — `make ci-start-align-check` |
| 4 | `syscall_register()` | `num >= MAX_SYSCALLS` silently discarded; NSI 80+ would have vanished | DDR-823 — panic + table 80→128 |

**Rule earned: when a check discards input rather than rejecting it, the discard
must be loud.**

---

## 4. Work queue

| # | Item | Blocked by |
|---|---|---|
| ✅ 0 | DDR-823 harness host-env detection (OPEN-9 misattribution) | done this session |
| 1 | **OPEN-10 root cause** — `smoke-sfs-btree` × 100 under `QEMU_SMP=4`; >2% ⇒ real race | — **do next; gates promotion** |
| 2 | Promote `dev/phase1` → `main` (two greens on one tip) | OPEN-10 verdict |
| 3 | `smoke-x25519` re-verify on a clean host, `TIMEOUT_S=300` | OPEN-9/10 settled |
| 4 | DDR-821 Ed25519 (RFC 8032) | — |
| 5 | DDR-813 ACC | x25519 + ed25519 gates green **in main** |
| 6 | B#3 / DDR-806 `-smp 4` virtio-blk stall | — (parallel, different surface) |
| 7 | Agent skill prompts × 8 in SFS | — (no kernel change) |
| 8 | DDR-824 ISO pipeline | **x86_64 100% gated first** |

---

## 5. August 31 — honest assessment

**28 days. ~73 features remaining.**

Highest-risk, each plausibly multi-session:

- `smoke-x25519` unknown failure — **blocks the entire ACC/AGS crypto chain**
- B#3 `-smp 4` virtio-blk stall — last 🔴 in Phase 3
- aarch64 / riscv64 ports — feature branches empty
- ISO pipeline — not started, 3–5 sessions on its own
- VirtualBox runner — not started
- Phase 9 assembly + cycle counts — 14%
- `prad` package manager — not started

### Risk flags, stated plainly

1. **The measured rate does not reach 73 features in 28 days.** The last two
   sessions produced ~2 features each and four infrastructure fixes. The
   infrastructure work was necessary — without DDR-822 every local
   verification was potentially against a stale binary — but it is not
   feature throughput.
2. **Four silent-drop defects in two sessions suggests more exist.** Each was
   found by accident while chasing something else, not by looking. Every one
   made some past "verified" claim weaker than it read.
3. **Some past greens meant less than recorded.** The eight gates DDR-817 found
   had never run; the probes DDR-822 found were tested stale. The features are
   probably fine — local 3-arm A/B is real evidence — but "two CI greens" did
   not mean what it was taken to mean for those slices.
4. **The remaining 26% is not the easy 26%.** It is disproportionately the
   never-started items (ISO, VirtualBox, package manager, arch ports, Phase 9),
   which have no scaffolding at all.

The honest read: **August 31 for all 286 items is not achievable at the observed
rate.** What is achievable is a defensible x86_64 build with the crypto chain
closed and the two open races fixed. If the deadline is fixed, the useful
decision is which subset ships — and that is a scope call, not an engineering
one.
