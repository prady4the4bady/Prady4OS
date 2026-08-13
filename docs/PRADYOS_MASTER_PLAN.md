# PRADYOS — COMPLETE, VERIFIED, PHASE-WISE MASTER PLAN

> **Single file. Nothing omitted.**
> Synthesised from `docs/BUILD_TRACKER.md`, `docs/AETHER_MASTER_FEATURES.md`,
> `docs/RECOVERY_AUDIT.md`, and all active session state as of 2026-08-14.
> All status claims are anchored to a CI run, a commit SHA, or an explicit
> "not verified" flag. Do not infer anything not stated here.

---

## ⚡ AUTONOMOUS CONTINUATION INSTRUCTIONS FOR CLAUDE

> **READ THIS FIRST. This section tells you exactly what to do, in order,
> without human input. Follow it precisely. Do not re-litigate anything
> in the Refuted Hypotheses table. Do not re-run STEP C — it is done.**

### Context-Limit Protocol
When you feel your context filling, **commit immediately** with an honest message,
append a checkpoint block to `SESSION_HANDOFF.md`, and stop. Do not summarise
without committing. The next session reads this file to resume.

### Repo Hygiene Rules (enforced before any new work)
1. Never insert a new NSI number mid-table. Append only. Next free NSI: **94**.
2. Never insert a new capability bit mid-bitmask. Next free: **1<<24**.
3. Every new smoke gate must have a stated timeout and a discriminating assertion
   (a gate that passes against a broken implementation is worthless).
4. Every DDR number must be unique and appended to the existing sequence.
5. `make ci-shard-check` must pass before any push.
6. `make ci-probe-rodata-check` must pass before any push.
7. `make ci-start-align-check` must pass before any push.
8. After any kernel header change, run `make clean && make` — never trust incremental.
9. The structural defect pattern has 16 instances. Before writing any new check,
   ask: "does this discard invalid input silently?" If yes, make it loud.

### Current Work — ORDERED, DO NOT REORDER

**TASK 3 (ACTIVE) — Fix DDR-782 kernel O_APPEND**

This is the ONE thing blocking `smoke-shell`. All character-loss hypotheses are
refuted. The bug is deterministic and localised:

- **Symptom:** `[shell] FAIL: 2>> truncated the earlier entry` — identical in all 3 arms
- **Shell side:** `user/prism.c:486` already passes `O_CREAT|O_WRONLY|O_APPEND`
  for `2>>`. Parser at `:446/:460` sets the flag correctly. Shell is NOT the bug.
- **Kernel side:** `kernel/syscall/sys_file.c` (and the FD_VFS write path) does
  not honour `O_APPEND`. The fd_entry has no `FD_APPEND` flag. Every write goes
  to the stored offset, not EOF. This is the DDR-782 gap.
- **Fix specification:**
  1. Add `FD_APPEND` flag to `struct fd_entry` in `kernel/syscall/sys_file.c`
     (or wherever fd_entry is defined — check `kernel/fs/vfs/vfs.h`).
  2. In `sys_open`: if `flags & O_APPEND`, set `entry->flags |= FD_APPEND`.
  3. In `sys_write` (the VFS write path): if `entry->flags & FD_APPEND`,
     acquire the VFS lock, seek `entry->off = entry->file->size`, then write.
     The seek + write must be atomic under the lock — this IS the POSIX
     atomicity property.
  4. Do NOT add a new syscall. Do NOT change any on-disk format.
     Do NOT change `CAP_FS_WRITE` — it already gates create/write.
  5. The existing `prism.c` `>>` (stdout append) uses `O_APPEND` too and must
     continue to work. The fix is generic — it benefits both `>>`  and `2>>`.
- **Gate:** `smoke-shell` — run 3 consecutive passes locally before pushing.
  The gate already has the discriminating assertion (`2>>` requires BOTH records
  to survive). A pass means O_APPEND is atomic.
- **Commit message:** `kernel: implement FD_APPEND / O_APPEND in sys_write (DDR-782)`

**TASK 4 — B#3 `-smp 4` thread loss (OPEN-10)**

Do this BEFORE any more OPEN-10 diagnosis work.
- Insert `kprintf("[tick] g_ticks=%lu at main.c:1134\n", g_ticks);` at `main.c:1134`
- Insert `kprintf("[tick] g_ticks=%lu at main.c:1311\n", g_ticks);` at `main.c:1311`
- Run `smoke-sfs-btree-smp4` 5× under `-smp 4`
- If the second stamp never appears, `fs_test_thread` is lost in the ~30 SFS I/O
  calls between those two lines. That confirms the B#3 diagnosis and the fix
  is a bounded timeout on `vfs_write` returning to the caller.
- Commit message: `kernel: add g_ticks stamps for B#3 OPEN-10 diagnosis (DDR-806)`

**TASK 5 — Push and CI**

```
git push origin dev/phase1
# wait for 3 consecutive CI greens on dev/phase1
git push origin dev/phase1:main   # NOT --ff-only; main is not an ancestor
```

**TASK 6 — Section F #66–76 (Visionary features)**

Work in DDR order. Each feature needs its own DDR number (next: DDR-884+).
Before implementing any F# item, run the Architecture Prerequisite Checklist
(Section 11 of this file). Start with **F#68 smoke-lockbox-e2e** because the
kernel and Python sides already exist — only the end-to-end gate is missing.

**TASK 7 — Section G remaining 4 agents**

`subconscious_agent`, `ai_scientist_agent`, `architect_agent`, `tournament_agent`
need kernel roster slots before their skill files mean anything.
Pattern: follow DDR-707 / DDR-737 for adding a named slot.

**TASK 8 — ISO pipeline**

x86_64: `multiboot2` header + `grub-mkrescue` → `pradyos-x86_64.iso`
aarch64: EFI + U-Boot packaging
riscv64: OpenSBI + U-Boot packaging
Apple Silicon: m1n1 shim over the aarch64 kernel

**TASK 9a–9e — TASK 17–21 (see §8 Work Queue)**

Order: TASK 17 (ISO) → TASK 18 (`prad`, NSI 88–90) → TASK 19 (Phase 9 asm) →
TASK 20 (invariant gates S1–S8, unblock S3/S7 by completing F#66–72 first) →
TASK 21 (v1.0.0 release tag).

---

## 0. Session Snapshot (as of 2026-08-14, commit 7aa4f31)

| Field | Value |
|---|---|
| **Local HEAD** | `7aa4f31` (committed, not pushed) |
| **Remote `dev/phase1` HEAD** | `3065e78` |
| **`main` HEAD** | `3b4830a` (promoted: runs 30804476970 / 30811210244 / 30811221820) |
| **CI status** | pending — nothing pushed since `bb7f9bc` |
| **Last committed content** | STEP C: `shell_evidence.sh` tees make stdout; three arms reveal one identical deterministic assertion: `[shell] FAIL: 2>> truncated the earlier entry (DDR-868)` |
| **NSI max** | **93** used (next free **94**) |
| **CI gates assigned** | 122 across 6 shards · 5 excluded |
| **Overall completion** | 217 ✅ / 25 ⚠️ / 28 ❌ out of 286 = **76%** |

### Tasks Done This Session
`0b` · `0c` · `1a` · `1b` · `1c` · `1d` · `1e` · `2` · `5` · **STEP C**

### Tasks Remaining
`3` (DDR-782 O_APPEND — THE ACTIVE TASK) · `4` · `6` · `7` · `8` · `9a–9e`

### Refuted Hypotheses — DO NOT RE-LITIGATE ANY OF THESE

| Hypothesis | Refutation |
|---|---|
| RTL trigger-level (STEP A: outb COM1+2, 0xC1) | `console.c:136` already sets FCR=0x07 (bits 7:6=00 = 1-byte trigger). 0xC1 sets 14-byte trigger — 14× worse. Not applied. |
| RX starvation | Refuted by RX-ring restoration commit |
| Feeder desync | Refuted by DDR-916 burst-start drain |
| Missing fixture | Not present in tree |
| THRE cap | Bounded by `CONSOLE_THRE_MAX` (DDR-809) |
| IRQ4 sharing / minimal RX repro (Steps B/D/E) | ALL closed — they target character loss. `smoke-shell` does NOT fail on character loss. |
| `smoke-shell` is non-deterministic / timing race | **REFUTED by STEP C.** The assertion is identical in all 3 arms every run. The apparent non-determinism was `tail -1` sampling different trailing lines. The bug is deterministic. |
| DDR-808 character loss | NOT the failure mode. The gate fails on O_APPEND semantics, not lost characters. |
| `[shell] FAIL` root cause was known before STEP C | **FALSE.** The FAIL line went to Makefile stdout, not the serial log. Every diagnosis before STEP C was blind. |

---

## 1. Phase Summary (all 11 phases)

| Phase | Name | Items | ✅ | ⚠️ | ❌ | % |
|---|---|---|---|---|---|---|
| 0 | Toolchain & Build | 11 | 6 | 2 | 3 | 55% |
| 1 | Bootloader | 9 | 6 | 2 | 1 | 67% |
| 2 | NEXUS Kernel Core | 57 | 46 | 5 | 4 | 81% |
| 3 | Driver Framework | 33 | 24 | 1 | 5 | 73% |
| 4 | Filesystem Layer | 25 | 22 | 0 | 0 | 88% |
| 5 | Userspace Foundation | 40 | 31 | 2 | 2 | 78% |
| 6 | Crypto Stack | 9 | 5 | 2 | 0 | 61% |
| 7 | AETHER Agent Runtime | 50 | 41 | 8 | 1 | 82% |
| 8 | Sovereign Desktop | 30 | 27 | 1 | 2 | 90% |
| 9 | Assembly Optimization | 7 | 1 | 2 | 4 | 14% |
| 10 | Quantum Layer | 4 | 0 | 0 | 4 | FUTURE |
| **TOTAL** | | **286** | **217** | **25** | **28** | **76%** |

---

## 2. Section A — Kernel/OS Shipped Features (CI-green; do not re-litigate)

### Boot / Kernel / Drivers / Filesystem

- MBR two-stage boot → long mode → ring-0 C (`smoke`)
- Kernel relocated to 4 MiB, 768 KiB load window (DDR-733)
- GDT/IDT + exception panic; 8259 PIC ISA-only; LAPIC/APIC timer @100 Hz (DDR-714A)
- Buddy PMM, SLAB heap, higher-half VMM, per-process CR3, W^X + NX (ADR-003/007/021)
- **Kernel self W^X** — `vmm_protect_kernel()` re-stamps text RX / data NX after boot (DDR-757)
- **COW fork** — `vmm_fork_address_space_cow()` + `PTE_SW_COW` + PMM refcounts + `vmm_cow_fault()` (IMP-D; `smoke-cowfork`)
- **SHA-256** (DDR-811) — `kernel/crypto/sha256.c`; 4 FIPS 180-4 vectors; gate `smoke-sha256`
- **HMAC-SHA256 + HKDF-SHA256** (DDR-818) — `kernel/crypto/hkdf.{c,h}`; 3 RFC 5869 vectors; gate `smoke-hkdf`
- **SHA-512** (DDR-821) — `kernel/crypto/sha512.{c,h}`; 4 FIPS 180-4 vectors incl. 1M-`a` streamed; gate `smoke-sha512` A/B-verified
- **ChaCha20-Poly1305 AEAD** (DDR-819) — `kernel/crypto/aead.{c,h}`, RFC 8439; gate `smoke-aead` shard 4
- **X25519** (DDR-820) — `kernel/crypto/x25519.{c,h}`, RFC 7748; 4/4 host passes; gate `smoke-x25519` shard 3
- **Ed25519** (DDR-821) — gate `smoke-ed25519` PASSES; all RFC 8032 §7.1 vectors + tamper/wrong-key/non-canonical-S rejection
- **ACC — Authenticated Confidential Channel** (DDR-813) — `SYS_ACC_SEAL` (77) / `SYS_ACC_OPEN` (78); `PASS smoke-acc (151s)` run 30944847959
- **ACC session rotation** (DDR-815) — `SYS_ACC_ROTATE` (81); `PASS smoke-acc-rotate (120s)` run 30966987476
- **AGS — Agent Goal Signing** (DDR-814) — `SYS_GOAL_SIGN` (79) / `SYS_GOAL_VERIFY` (80); `PASS smoke-ags (120s)` run 30960084022
- **Secure credential vault** (DDR-834) — `SYS_VAULT_PUT` (87) / `SYS_VAULT_GET` (91); `PASS smoke-vault (120s)` run 30993915008
- **Agent memory** (DDR-836) — `SYS_MEMORY_WRITE` (82) / `SYS_MEMORY_READ` (83), `CAP_MEMORY` (1<<18); `PASS smoke-agentmem (120s)` run 31003118400
- **Agent checkpoint/resume** (DDR-837) — `SYS_CHECKPOINT_AGENT` (84) / `SYS_RESUME_AGENT` (85); `PASS smoke-checkpoint (120s)` run 31015668039
- **Spawn-depth cap** (DDR-838) — `SPAWN_DEPTH_MAX=3`, keyed on lineage `tcb.agent_depth`; `PASS smoke-spawndepth (120s)` run 31028810861
- **DAG action queue** (DDR-839) — `SYS_SUBMIT_CHILD_ACTION` (92); `PASS smoke-actiondag (120s)` run 31043474501
- **Code-rewrite approval** (DDR-842) — `SYS_APPROVE_CODE_REWRITE` (86), `CAP_REWRITE` + `CAP_SOVEREIGN`; `PASS smoke-coderewrite (120s)` run 31094358972
- **Tamper-evident audit chain** (DDR-842) — `SYS_VERIFY_AUDIT` (93); `PASS smoke-auditchain` + `PASS smoke-auditchain-tamper` run 31094358972
- **Metric lockbox** (DDR-812) — `SYS_METRIC_READ` (76); `-ETAMPER` on mismatch; gates `smoke-lockbox` + `smoke-metric`
- **Kernel entropy** (DDR-816) — virtio-rng primary, RDSEED secondary; gate `smoke-rng`
- **Console input integrity** (DDR-809) — `CONSOLE_THRE_MAX`, RX FIFO drain inline, RX ring multi-producer; closes OPEN-8
- Per-CPU runqueues + work-stealing scheduler; SMP 4 APs (ADR-029/031)
- MSI-X for all virtio devices (DDR-714C/771); multi-in-flight virtio-blk (DDR-BLK-1)
- NCS capabilities: `CAP_SOVEREIGN`, `CAP_AGENT`, `CAP_NET` (ADR-009, DDR-731)
- NIA IPC sync/async/broadcast (ADR-010/011)
- FAT32 RW + VFAT LFN (ADR-015/020); SFS CoW B+tree, extents, journal, LZ4, snapshots (ADR-018)
- SFS hierarchical dirs (DDR-738), unlink/rmdir (DDR-741), free-space GC (DDR-762-v2)
- SFS write-budget token bucket 25 MiB/s (ADR-032); cross-reboot persistence (DDR-768/769/770)
- Host `mkfs.sfs` + multi-leaf B+tree bulk load (DDR-767/773); ext4 read-only (ADR-019); per-process root mount (DDR-739)
- NVMe controller + block I/O (DDR-765/766/772); `VBLK_MAX` 4→8 (DDR-771)
- NVMe MSI-X (DDR-774a/b/c) — programmed + delivered + gated (`smoke-nvme`, `[nvme] irqs=6`); IRQ-driven sleep deferred
- **aarch64 / riscv64 bootstrap** (ADR-034) — boot-only; CI-green every run
- **Bounded W^X carve-out** for self-rewriting code (ADR-035)

### Userspace / Syscalls / Shell

- Static ELF64 loader + per-process W^X AS (ADR-021); musl libc v1.2.5 (ADR-023/033)
- `pradyos-init` PID 1 + orphan reaper; PRISM shell with full-register fork (ADR-024)
- PRISM redirection `>` `>>` `<` `2>` and pipes `|` `a|b|c` (DDR-778/780/781/782/784/786/787)
- Blocking pipe semantics (DDR-787); exit status `$?` (DDR-789)
- PRISM builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit
- `ls` via `SYS_GETDENTS` (66); `ps` via `SYS_GETPROCS` (67)
- Lazy per-thread FPU save/restore; copyin/copyout (EFAULT never panics)
- NSI 1–93 allocated; next free **94**
- TCP loopback echo; kill end-to-end
- CI harness early-exit (DDR-785/788); `make smoke-selftest`; per-boot probe selection (DDR-804)
- `smoke-chipset` — q35/qemu64, pc/qemu64 (i440FX), q35/Nehalem, q35/Opteron_G5

### ⚠️ KNOWN GAP — O_APPEND not yet in kernel (ACTIVE BUG)

`prism.c:486` passes `O_APPEND` correctly for both `>>` and `2>>`. The kernel
does not yet honour it. `sys_write` always writes at the stored fd offset,
not at EOF. This causes `2>>` to truncate the file. Fix is TASK 3 (DDR-782).

### AETHER Agent Layer (kernel plumbing)

- Kernel action queue + append-only audit log (ADR-026); per-process mem cap + syscall rate limit
- 10 NSI agent calls (29–38); ring-3 daemon + `agent_base.c`
- Ring-3 socket NSI, 8 proxy sockets, live Ollama over HTTP (ADR-027)
- AETHER boot config from `/etc/aether/config` via SFS (DDR-732/770)
- `CAP_NET` allowlist, deny-by-default egress (DDR-734); agent CPU metrics (DDR-735/736)
- Per-agent live metrics, post-mortem stable (DDR-730); `SYS_AGENT_ROSTER` 8 named slots (DDR-707)
- 8 named agents KRYOS…SOLIN with UI panel cards + action pips (DDR-737)
- Egress audit R1 + R3 (DDR-800/801); privacy-mode netfilter (DDR-802)
- Cloud bridge built, **not enabled** (DDR-793 R1/R3 pending)
- Docker reproducible build env (`Dockerfile` + `make docker-build`)
- VirtualBox runner (`tools/vbox_runner/run_vbox.sh`; exits 77 when VBox absent)

### Layer 7 UI / Sovereign Desktop

- VirtIO-GPU framebuffer (ADR-028); ring-3 FB surface `SYS_FB_INFO/MAP/FLUSH` (DDR-702)
- PS/2 keyboard `SYS_INPUT_POLL` (DDR-703); virtio-input pointer `SYS_MOUSE_POLL` (DDR-705)
- Sovereign/Manual mode toggle (DDR-701); compositor w/ 8×8 font (DDR-704)
- Per-client surfaces `SYS_SURFACE_*` (DDR-706); z-order/focus/key routing (DDR-708)
- Sun-driven OKLab ambiances (DDR-709); window drag/close/resize/minimize/maximize (DDR-710/711/717/719)
- Glass blur, gradients, particle field, decorations, alt-tab, page flip, scroll, spring/ripple, Inter font (DDR-712/720–728)
- Surface destroy lifecycle safety (DDR-729); agent-card click → `SYS_SPAWN_AGENT` (DDR-713)

---

## 3. Section A2 — AETHER Host-Side Python Agent Layer (ASI-bridge v4.0)

> Root: `aether/` · Python 3.13 · Runs on top of the OS, not inside the kernel.
> Gate: `python -m pytest -W error -x -q aether/tests/` (CI job `aether-layer`)
> Invariants: S1–S14 in `aether/kernel/invariants/core_invariants.py` — **independent of kernel S1–S8, never merge**.

| Module | File | Status |
|---|---|---|
| B-01 CAP_SOVEREIGN lockbox | `aether/kernel/lockbox/cap_sovereign.py` | ✅ |
| B-02 append-only audit log | `aether/kernel/audit/audit_log.py` | ✅ |
| B-03 Merkle integrity | `aether/kernel/integrity/merkle.py` | ✅ |
| B-04 prompt-injection firewall | `aether/agents/firewall/prompt_injection.py` | ✅ |
| B-05 quarantine namespace | `aether/agents/quarantine/namespace.py` | ✅ |
| B-06 red-team agent | `aether/agents/red_team/red_team_agent.py` | ✅ |
| B-07 agent runtime bus | `aether/agents/runtime/agent_bus.py` | ✅ |
| B-08 notification bus | `aether/agents/notification/notif_bus.py` | ✅ |
| B-09 Ollama model manager | `aether/ai_core/model_manager/manager.py` | ✅ |
| B-10 computer-use interface | `aether/ai_core/computer_use/interface.py` | ✅ |
| B-11 out-of-box experience | `aether/platform/oobe/oobe.py` | ✅ |
| B-12 AgentNet P2P mesh | `aether/agents/agentnet/mesh.py` | ✅ |
| B-13 soul/personality engine | `aether/agents/soul/soul_engine.py` | ✅ |
| B-14 vision pipeline | `aether/vision/pipeline.py` | ✅ |
| B-15 self-model maintenance | `aether/agents/self_model/self_model.py` | ✅ |
| B-16 introspective failure analysis | `aether/agents/introspection/failure_analysis.py` | ✅ |
| B-17 blind-spot discovery loop | `aether/agents/blindspot/discovery_loop.py` | ✅ |
| C-01 federated knowledge sync | `aether/agents/federation/knowledge_sync.py` | ✅ |
| C-02 distributed experiments | `aether/agents/federation/distributed_experiments.py` | ✅ |
| C-03 emergent tool composition | `aether/agents/tool_composer/composer.py` | ✅ |
| C-04 invention registry | `aether/agents/invention/registry.py` | ✅ |
| C-05 capability boundary mapper | `aether/agents/capability/boundary_mapper.py` | ✅ |
| C-06 counterfactual simulator | `aether/agents/counterfactual/simulator.py` | ✅ |
| C-07 NL constraint spec + enforcer | `aether/agents/constraints/nl_constraint_parser.py` | ✅ |
| C-08 offline safety review mode | `aether/agents/safety/offline_review.py` | ✅ |
| C-09 versioned agent contracts | `aether/agents/contracts/agent_contract.py` | ✅ |
| C-10 cross-domain analogy engine | `aether/agents/analogy/cross_domain_engine.py` | ✅ |
| D-01 self-improvement proposals | `aether/agents/self_improvement/proposal_engine.py` | ✅ |
| D-02 long-horizon planner | `aether/agents/planner/long_horizon_planner.py` | ✅ |
| D-03 multi-model ensemble router | `aether/ai_core/ensemble/ensemble_router.py` | ✅ |
| D-04 persistent world model | `aether/agents/world_model/world_model.py` | ✅ |
| D-05 meta-learning controller | `aether/agents/meta_learning/meta_controller.py` | ✅ |
| D-06 causal reasoning engine | `aether/agents/causal/causal_reasoning_engine.py` | ✅ |
| D-07 hypothesis generator | `aether/agents/research/hypothesis_generator.py` | ✅ |
| D-08 superalignment monitor | `aether/agents/safety/superalignment_monitor.py` | ✅ |
| D-09 recursive self-model updater | `aether/agents/self_model/recursive_self_model_updater.py` | ✅ |
| D-10 experiment outcome evaluator | `aether/agents/research/experiment_outcome_evaluator.py` | ✅ |
| D-11 knowledge consolidation | `aether/agents/memory/knowledge_consolidation.py` | ✅ |
| D-12 adaptive goal prioritizer | `aether/agents/planner/adaptive_goal_prioritizer.py` | ✅ |
| D-13 failure memory registry | `aether/agents/memory/failure_memory_registry.py` | ✅ |
| D-14 lineage knowledge loader | `aether/agents/memory/lineage_knowledge_loader.py` | ✅ |
| D-15 autonomous skill composer | `aether/agents/tool_composer/autonomous_skill_composer.py` | ✅ |
| I-09 alignment wiring | `aether/agents/safety/alignment_wiring.py` | ✅ |
| I-02 capability enforcement | `aether/capability/enforcement.py` | ✅ |
| I-10 daemon coordinator | `aether/daemon/coordinator.py` | ✅ |
| DDR-792 ollama transport bridge | `aether/ollama_bridge/transport.py` | ✅ |
| F#68 metric lockbox (S3) | `aether/kernel/lockbox/metric_lockbox.py` | ✅ |
| Privacy-mode netfilter hook | `aether/platform/privacy/netfilter.py` | ✅ |
| Shared egress rate limiter (S2) | `aether/platform/ratelimit/shared_limiter.py` | ✅ |
| Cloud bridge (built, NOT enabled) | `aether/cloud_bridge/transport.py` | ✅ |
| F#68 kernel wire (DDR-795) | `kernel/aether/metric_page.c` + `aether/kernel/lockbox/metric_region.py` | ✅ |
| I-01…I-10 integration wiring | — | ✅ COMPLETE |
| J-01…J-06 retro audit | — | ✅ verified DDR-845 |

---

## 4. Section B — Kernel/OS Planned Features

| # | Feature | Priority | Status |
|---|---|---|---|
| B#1 | NVMe IRQ (MSI-X) | High | ⏸ functionally complete (DDR-774c); IRQ-driven sleep deferred |
| B#3 | `-smp 4` percpu-sched / thread loss | **High** | ⬜ OPEN — DDR-806. Next: stamp `g_ticks` at `main.c:1134` and `:1311`. Gates promotion. |
| B#4 | SFS as default process root | Medium | ⬜ planned |
| B#6 | ext4 write support | Medium | ⬜ planned |
| B#9 | I/O APIC (q35 GSI routing) | Low | ⬜ planned |
| B#10 | Per-CPU runqueue affinity/NUMA hints | Low | ⬜ planned |
| B#12 | Job control in PRISM | Low | ⬜ partial (`$?` ✅, SIGPIPE ✅, `&` ⬜) |
| B#13 | Dynamic linker | Low | ⬜ planned |
| B#14 | 3-lane NAS scheduler | Low | ⬜ planned |
| B#15 | PMM variable-weight allocator | Low | ⬜ planned |

---

## 5. Open Defects

| ID | Symptom | Status |
|---|---|---|
| **DDR-868 / TASK 3** | `smoke-shell` FAIL: `2>>` truncates instead of appending | **ACTIVE — kernel O_APPEND not implemented. Fix specified in §0 TASK 3 above.** |
| **OPEN-1** | `smoke-surfdestroy` intermittently misses sentinel | open, passive |
| **OPEN-2** | Intermittent CI reds on `-smp 4` gates | open — DDR-863 |
| **OPEN-9** | `smoke-shell` fails locally / passes CI (QEMU lock-hold) | root cause not yet caught; **not the same failure as DDR-868** |
| **OPEN-10** | item-47 lost-thread failure seen through `smoke-sfs-btree-smp4` | open, gates promotion |
| **B#3 / DDR-806** | `-smp 4` thread loss | open — see §4 |
| ~~OPEN-7~~ | Per-boot probe selection | CLOSED (DDR-804) |
| ~~OPEN-8~~ | Console input loss | CLOSED (DDR-809) |
| ~~OPEN-11~~ | `smoke-sha256` after fresh image | CLOSED (DDR-831) |

### Structural Defect Pattern — 16 Instances

| # | Where | Silent drop | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran | DDR-817 |
| 2 | Makefile user sources | 14/31 probes stale | DDR-822 |
| 3 | `user/` `_start` attribute | silent #GP reintroduced | DDR-823 |
| 4 | `syscall_register()` | NSI 80+ discarded | DDR-823 |
| 5 | `check_global_forbidden()` | `op=` line dropped | DDR-824 |
| 6 | crypto sources not prerequisites | build never runs | DDR-825 |
| 7 | writable global in R+X probe | STORE faults at runtime | DDR-826 |
| 8 | PMM double-free | freed frame re-pooled | DDR-830 |
| 9 | mid-enum insertion | wire format renumbered | DDR-832 |
| 10 | kernel headers not prerequisites | third recurrence | DDR-833/835 |
| 11 | SkillOpt accepting TIE | skill drifts uncontrolled | DDR-847 |
| 12 | skill revision thinning refusals | optimiser rewarded for dropping | DDR-848 |
| 13 | `rates.get(model, 0.0)` | unpriced model = free | DDR-849 |
| 14 | `check_invariant()` tautology | invariant could never fail | DDR-849 |
| 15 | building from label not spec | items satisfied title, missed §3D | DDR-850 |
| 16 | mutation harness stale bytecode | kills counted from prior run | DDR-853 |

**Standing rule:** a check that discards input silently is a bug. Make every discard loud.

---

## 6. NSI Allocation Table (append-only)

| NSI | Syscall | Capability | Status |
|---|---|---|---|
| 1–28 | Core OS syscalls | varies | ✅ |
| 29–38 | AETHER agent calls | CAP_AGENT / CAP_SOVEREIGN | ✅ |
| 66 | SYS_GETDENTS | — | ✅ |
| 67 | SYS_GETPROCS | — | ✅ |
| 69/70 | SYS_POWEROFF / SYS_REBOOT | — | ✅ |
| 71–75 | SYS_SYSINFO/TIME/DMESG/MEMINFO/SETNAME | — | ✅ |
| 76 | SYS_METRIC_READ | CAP_SOVEREIGN | ✅ DDR-812 |
| 77 | SYS_ACC_SEAL | CAP_AGENT | ✅ DDR-813 |
| 78 | SYS_ACC_OPEN | CAP_SOVEREIGN | ✅ DDR-813 |
| 79 | SYS_GOAL_SIGN | CAP_SOVEREIGN | ✅ DDR-814 |
| 80 | SYS_GOAL_VERIFY | CAP_AGENT | ✅ DDR-814 |
| 81 | SYS_ACC_ROTATE | CAP_SOVEREIGN | ✅ DDR-815 |
| 82 | SYS_MEMORY_WRITE | CAP_MEMORY (1<<18) | ✅ DDR-836 |
| 83 | SYS_MEMORY_READ | CAP_MEMORY (1<<18) | ✅ DDR-836 |
| 84 | SYS_CHECKPOINT_AGENT | CAP_SOVEREIGN | ✅ DDR-837 |
| 85 | SYS_RESUME_AGENT | CAP_SOVEREIGN | ✅ DDR-837 |
| 86 | SYS_APPROVE_CODE_REWRITE | CAP_REWRITE + CAP_SOVEREIGN | ✅ DDR-842 |
| 87 | SYS_VAULT_PUT | CAP_SOVEREIGN | ✅ DDR-834 |
| 88–90 | `prad` package manager | — | ⬜ TASK 18 |
| 91 | SYS_VAULT_GET | CAP_SOVEREIGN | ✅ DDR-834 |
| 92 | SYS_SUBMIT_CHILD_ACTION | CAP_AGENT | ✅ DDR-839 |
| 93 | SYS_VERIFY_AUDIT | CAP_SOVEREIGN | ✅ DDR-842 |
| **94+** | **NEXT FREE** | — | — |

> ⚠️ `prad` was previously scoped to NSI 87–89. 87 is `SYS_VAULT_PUT`. Use **88–90**.

---

## 7. Capability Bits

| Bit | Name | Status |
|---|---|---|
| 1<<16 | CAP_SOVEREIGN | ✅ |
| 1<<17 | CAP_AGENT | ✅ |
| 1<<18 | CAP_MEMORY | ✅ DDR-836 |
| 1<<19 | CAP_OCR | ⬜ deferred post-1.0 |
| 1<<20 | CAP_EXEC | ⬜ deferred post-1.0 |
| 1<<21 | CAP_REWRITE | ✅ DDR-842; always requires CAP_SOVEREIGN |
| 1<<22 | CAP_SCENE | ⬜ deferred post-1.0 (post-L7) |
| 1<<23 | CAP_NET_BROWSE | ⬜ deferred post-1.0 |

---

## 8. Work Queue — Complete, Dependency-Ordered

Status: ✅ done · 🔵 in-progress · ⬜ not started · 🔒 blocked

### Immediate (TASK 3 is the active task)

| # | Task | Status |
|---|---|---|
| 0b | Checkpoint commit `bb7f9bc` | ✅ |
| 0c | SESSION_HANDOFF.md updated | ✅ |
| 1a–1e | STEP C through push | ✅ STEP C done (`7aa4f31`); push pending |
| 2 | CI green | 🔒 needs push |
| **3** | **DDR-782 kernel O_APPEND — see §0 TASK 3** | 🔵 **ACTIVE** |
| 4 | B#3 g_ticks stamps | ⬜ |
| 5 | Push to `main` (3 greens; `git push origin dev/phase1:main`) | 🔒 |
| 6 | Section F #66–76 | ⬜ start with F#68 smoke-lockbox-e2e |
| 7 | Section G remaining 4 agents | ⬜ |
| 8 | ISO pipeline | ⬜ |
| 9a | TASK 17 ISO | ⬜ |
| 9b | TASK 18 `prad` NSI 88–90 | ⬜ |
| 9c | TASK 19 Phase 9 asm | ⬜ |
| 9d | TASK 20 invariant gates S1–S8 | ⬜ unblock S3/S7 by completing F#66–72 first |
| 9e | TASK 21 v1.0.0 release | ⬜ |

### Group 1 — Tracker & Build-System Integrity

| # | Item | Status |
|---|---|---|
| G1-1 | NSI 87 collision fix | ✅ DDR-840 |
| G1-2 | Docker reproducible build | ✅ |
| G1-3 | CMake skipped v1.0.0 | ✅ DDR-843 |
| G1-4 | VirtualBox runner | ✅ |
| G1-5 | chipset coverage | ✅ `smoke-chipset` |

### Group 2 — Section E / Agent Core Close-Out

| # | Item | Status |
|---|---|---|
| G2-1 | NSI 86 + CAP_REWRITE | ✅ run 31094358972 |
| G2-2 | Audit hash chain + NSI 93 | ✅ run 31094358972 |
| G2-3 | 3C action types (8/14; 6 deferred) | ✅ partial |
| G2-4 | 3D daemon features #45–65 | ✅ ALL 21 COMPLETE |
| G2-5 | Section F #66–76 | ⬜ |
| G2-6 | Section G 12-agent roster | ⬜ partial |
| G2-7 | J-01…J-06 retro audit | ✅ DDR-845 |
| G2-8 | smoke-invariants S1–S8 | ✅ run 31104672684 |
| G2-9 | smoke-aead gate wiring | ✅ shard 4 |

#### 3C Action Types — 6 Deferred (on record)

| Action | Why |
|---|---|
| ACTION_CAPTURE_FRAME | post-L7; CAP_SCENE + camera path missing |
| ACTION_SCAN_ENVIRONMENT | post-L7; SLAM3R missing |
| ACTION_QUERY_SCENE | post-L7; scene graph missing |
| ACTION_PARSE_DOCUMENT | 64 MiB OCR model; no model-shipping path |
| ACTION_EXEC_CODE | needs sandboxed interpreter subsystem |
| ACTION_BROWSE_WEB | headless browser + network egress; DEFERRED post-1.0 (DDR-843) |

#### Section 3D — #45–65 — ALL 21 COMPLETE

| # | Item | DDR |
|---|---|---|
| #45 | skill.md 8 roster files | DDR-846 |
| #46 | SkillOpt loop | DDR-847 |
| #47 | SkillOpt-Sleep | DDR-848+850 |
| #48 | skill-update validation | DDR-848+850 |
| #49 | multi-agent skill transfer | DDR-848 |
| #50 | TokenJuice | DDR-849+850 |
| #51 | JSONL trajectory log | DDR-849 |
| #52 | cost accounting | DDR-849+850 |
| #53 | goals.md per agent | DDR-851 |
| #54 | subconscious loop | DDR-851 |
| #55 | MOSS pipeline | DDR-851 |
| #56 | OCR→memory | DDR-852 |
| #57 | multi-modal context builder | DDR-852 |
| #58 | privacy mode ring-3 | DDR-852 |
| #59 | model routing | DDR-852 |
| #60 | hypothesis tree | DDR-853+855 |
| #61 | genome.md | DDR-853 |
| #62 | vector knowledge graph | DDR-856 |
| #63 | dead-end registry | DDR-855 |
| #64 | population tournament | DDR-856 |
| #65 | replayable run visualiser | DDR-856 |

### Section F — Visionary #66–76 (TASK 13)

| # | Feature | Status |
|---|---|---|
| F#66 | architect_agent | ⬜ |
| F#67 | healer_agent | ⬜ |
| F#68 | metric lockbox e2e | ⚠️ kernel+Python ✅; smoke-lockbox-e2e ⬜ |
| F#69 | inventor_agent | ⬜ |
| F#70 | tournament_agent | ⬜ |
| F#71 | subconscious world model | ⬜ |
| F#72 | verifier_agent | ⬜ |
| F#73 | sovereign NL UI | ⬜ |
| F#74 | capability discovery | ⬜ |
| F#75 | lineage memory | ⬜ |
| F#76 | tamper-evident ledger | ⬜ |

### Section G — 12-Agent Roster (TASK 14)

| Role | Slot | Spawnable? |
|---|---|---|
| file_agent | KRYOS | ✅ |
| shell_agent | PRAX | ❌ CAP_EXEC unwired |
| research_agent | LUMYN | ❌ CAP_NET_BROWSE unwired |
| ocr_agent | AHNIS | ❌ CAP_OCR unwired |
| vision_agent | IRIS | ❌ CAP_SCENE unwired (post-L7) |
| healer_agent | RUFLO | ❌ not yet spawnable |
| orchestrator_agent | HERMES | ✅ |
| verifier_agent | SOLIN | ✅ |
| subconscious_agent | — | ⬜ no kernel slot yet |
| ai_scientist_agent | — | ⬜ no kernel slot yet |
| architect_agent | — | ⬜ no kernel slot yet |
| tournament_agent | — | ⬜ no kernel slot yet |

### Section B Remaining (TASK 16)

B#1 ⏸ · B#4 ⬜ · B#6 ⬜ · B#9 ⬜ · B#10 ⬜ · B#12 ⬜ · B#13 ⬜ · B#14 ⬜ · B#15 ⬜

### TASK 17 — ISO Pipeline

| Target | Boot | ISO |
|---|---|---|
| x86_64 | ✅ | ⬜ multiboot2 + grub-mkrescue |
| aarch64 | ✅ CI | ⬜ EFI/U-Boot |
| riscv64 | ✅ CI | ⬜ OpenSBI + U-Boot |
| Apple Silicon | ⬜ | ⬜ m1n1 shim |

### TASK 18–21

| Task | Item | Status |
|---|---|---|
| 18 | `prad` NSI **88–90** | ⬜ |
| 19 | Phase 9 assembly | ⬜ |
| 20 | Invariant gates S1–S8 | ⬜ partial |
| 21 | v1.0.0 release | ⬜ |

---

## 9. Section D — ADR-026 Baseline (#1–17) — VERIFIED BUILT

All 17 items confirmed in `kernel/aether/`. No drift found.

| # | Item | Status |
|---|---|---|
| 1 | Kernel-is-arbiter (`SYS_SUBMIT_ACTION`) | ✅ |
| 2 | Sovereign auto-approve | ✅ |
| 3 | Manual mode 60 s expiry | ✅ |
| 4 | 256-entry fixed queue | ✅ |
| 5 | 4096-entry append-only audit | ✅ |
| 6 | 128 MiB cap + OOM kill | ✅ |
| 7 | CAP_SOVEREIGN (1<<16) | ✅ |
| 8 | CAP_AGENT (1<<17) | ✅ |
| 9 | 60 syscall/s rate limiter | ✅ |
| 10 | SYS_SPAWN_AGENT force-PENDING | ✅ |
| 11 | SYS_KILL_AGENT own-children only | ✅ |
| 12 | Ollama HTTP/1.1 ring-3 bridge | ✅ |
| 13 | CI test mode deterministic | ✅ |
| 14 | Daemon topology PID-1→daemon→agents | ✅ |
| 15 | ACTION_WRITE_FILE (SFS W^X) | ✅ |
| 16 | ACTION_PRINT (512 B bounded) | ✅ |
| 17 | ACTION_SPAWN_PROCESS force-PENDING | ✅ |

---

## 10. Section H — Security Invariants (BINDING, ADR-level)

- **S1** — No self-escalation. Agent cannot raise own caps, `mem_limit`, or approve own actions.
- **S2** — Bounded everything. queue 256 · audit 4096 · payload 512 B · memory 128 MiB · syscalls 60/s · skill 2 KB · IPC 256 B · spawn depth 3 · lineage ≤16/query. Every bound: error or clean kill, never panic.
- **S3** — Immutable objective function. CAP_SOVEREIGN-locked SFS path; agent write → kill + audit.
- **S4** — Human gate is structural. spawn/scene/skill-update/code-rewrite/genome-evolve/architect → CAP_SOVEREIGN; force-PENDING for highest-consequence.
- **S5** — Append-only audit + Merkle ledger. No user-space erase; wraps flagged.
- **S6** — Fault isolation. Ring-3 fault kills agent, not kernel; copyin/copyout on all user pointers.
- **S7** — Metric-gaming prevention. Verifier agent structurally independent; objective function kernel-locked.
- **S8** — Skill-change veto. Any `skill.md` or code write requires CAP_SOVEREIGN.

---

## 11. Architecture Prerequisite Checklist (answer in DDR before any Section E/F item)

1. NSI range open? Next free: **94**
2. TCB / roster-slot fields — struct extension? ABI size assumptions?
3. PMM/VMM shared mappings — `PTE_SW_SHARED` or new class?
4. Capability check — which `CAP_*` gates it?
5. AETHER queue/audit — new record type needed?
6. Scheduler hooks — `sched_exit`, spawn-depth enforcement?
7. FS/root-mount constraints — depends on `root_mnt`, SFS dirs?
8. Network policy — touches `net_allow`, socket NSI?
9. UI exposure — new agent card needed?
10. Smoke gate — what proves it **deterministically**?
11. **S1–S8 check (MANDATORY)** — which invariants govern it? Does the plan violate any, even indirectly?

---

## 12. Risk Flags

1. **Rate does not cover remaining work.** ~69 features remain. ~2 features + infra per session.
2. **Silent-drop defects** — 16 found, more likely exist. Every one was found while chasing something else.
3. **Remaining work is disproportionately never-started** — Sections F/G, `prad`, Phase 9, invariant gates. No scaffolding.
4. **ISO task is cheaper than it looked** — packaging over already-booting targets.
5. **Honest v1.0.0 scope:** x86_64 with crypto chain closed, both `-smp 4` races fixed, ISOs for 3 booting targets.

---

## 13. Competitor Landscape

| Competitor | Weakness | AETHER fix |
|---|---|---|
| OpenClaw (210k★) | Skill-vetting security | CAP_AGENT sandbox + CAP_SOVEREIGN veto + audit |
| AutoGPT (183k★) | Stalled, hallucination-prone | SkillOpt-Sleep + MOSS + SFS CoW rollback |
| CrewAI (60k★) | No kernel isolation | ACTION_SEND_IPC + DAG queue at kernel level |
| LangChain (146k★) | Stateless, cloud-dependent | SYS_MEMORY_WRITE/READ + SFS + local Ollama |
| n8n (100k★) | Static workflows | Subconscious agent rewrites on failure + SkillOpt |
| Dify (85k★) | Weak action capability | ACTION_EXEC_CODE + ACTION_PARSE_DOCUMENT |
| OpenHands (47k★) | Privileged Docker, no self-improvement | ring-3 isolation + kernel-audited syscalls |
| MOSS (academic) | No kernel safety on self-rewriting | ACTION_REWRITE_AGENT_CODE behind CAP_SOVEREIGN |
| Ouroboros/AutoResearch | No local fallback, metric-gaming risk | Local Ollama + kernel-locked metric lockbox |
| AI Scientist/Curie | $15–45/paper, no persistent memory | $0 local experiments + persistent hypothesis tree |
| Pentagi (19k★) | Security-domain only | Same CAP_AGENT sandbox + rate limiting |
| RuVector (4k★) | Standalone DB, no agent loop | Adopted as AETHER vector knowledge-graph model |

---

## 14. Standing AETHER Cognitive/ASI Mandate

Long-term: subconscious background reasoning (F#71), verifier-based self-improvement
(F#72), metric lockbox (F#68/S3 — highest priority among proposed features), and
the full 12-agent Section G roster. Decompose into bounded DDRs only when a
concrete slice is chosen. Section D is confirmed built — that is the precondition
for all Section E/F work.

---

*Last updated: 2026-08-14 — STEP C breakthrough recorded. Local HEAD `7aa4f31` / remote `3065e78` / `main` `3b4830a`. Active task: DDR-782 kernel O_APPEND.*
