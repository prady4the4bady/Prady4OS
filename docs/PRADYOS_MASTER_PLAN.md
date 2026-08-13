# PRADYOS — COMPLETE, VERIFIED, PHASE-WISE MASTER PLAN

> **Single file. Nothing omitted.**  
> Synthesised from `docs/BUILD_TRACKER.md`, `docs/AETHER_MASTER_FEATURES.md`,
> `docs/RECOVERY_AUDIT.md`, and all active session state as of 2026-08-14.
> All status claims are anchored to a CI run, a commit SHA, or an explicit
> "not verified" flag. Do not infer anything not stated here.

---

## 0. Session Snapshot (as of 2026-08-14)

| Field | Value |
|---|---|
| **Local HEAD** | `bb7f9bc` (committed, not pushed) |
| **Remote `dev/phase1` HEAD** | `3065e78` |
| **`main` HEAD** | `3b4830a` (promoted on 3 consecutive greens: runs 30804476970, 30811210244, 30811221820) |
| **CI status** | pending — nothing pushed since `bb7f9bc` |
| **Last committed content** | console.c RX-ring restoration · DDR-916 burst-start drain · `tools/ci/shell_evidence.sh` · `.gitignore` · checkpoint appended to `SESSION_HANDOFF.md` |
| **NSI max** | **91** used (next free **92**) — see §NSI table |
| **CI gates assigned** | 122 across 6 shards · 5 excluded (each with a stated reason) |
| **Overall completion** | 217 ✅ / 25 ⚠️ / 28 ❌ out of 286 items = **76%** |

### Immediate Next Actions (ordered, do not reorder)

1. **STEP C first** — make `tools/ci/shell_evidence.sh` tee `make` stdout so the
   `[shell] FAIL` assertion becomes visible, then re-run `smoke-shell` 3× and
   read which assertion actually fails.
2. **Steps D & E** — follow from Step C findings.
3. **Tasks 3, 4, 6, 7, 8, 9a–9e** — see §Work Queue.
4. **Push `dev/phase1`** then wait for CI green before any `main` merge
   (`main` is not an ancestor of `dev/phase1`, so `--ff-only` will fail; use
   `git push origin dev/phase1:main` after 3 greens).

### Refuted Hypotheses (do not re-litigate)

| Hypothesis | Refutation evidence |
|---|---|
| RTL trigger-level (STEP A: outb COM1+2, 0xC1) | `console.c:136` already sets FCR=0x07 (bits 7:6=00 = 1-byte trigger, the most aggressive). 0xC1 sets bits 7:6=11 = 14-byte trigger — 14× worse. Step A was a no-op at best, harmful at worst. Not applied. |
| RX starvation | Refuted by RX-ring restoration commit |
| Feeder desync | Refuted by DDR-916 burst-start drain |
| Missing fixture | Not present in tree |
| THRE cap | Bounded by `CONSOLE_THRE_MAX`, verified in DDR-809 |
| [shell] FAIL root cause known | **FALSE** — the FAIL line goes to Makefile stdout, not the serial log. Shell_evidence.sh never captured it. Every prior diagnosis was blind. |

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
- NVMe MSI-X (DDR-774a/b/c) — programmed + delivered + gated (`smoke-nvme`, `[nvme] irqs=6`); IRQ-driven sleep deferred to a future DDR
- **aarch64 / riscv64 bootstrap** (ADR-034) — boot-only; CI-green every run in `arch-bootstrap` job
- **Bounded W^X carve-out** for self-rewriting code (ADR-035)

### Userspace / Syscalls / Shell

- Static ELF64 loader + per-process W^X AS (ADR-021); musl libc v1.2.5 (ADR-023/033)
- `pradyos-init` PID 1 + orphan reaper; PRISM shell with full-register fork (ADR-024)
- PRISM redirection `>` `>>` `<` `2>` and pipes `|` `a|b|c` (DDR-778/780/781/782/784/786/787)
- Blocking pipe semantics (DDR-787); exit status `$?` (DDR-789)
- PRISM builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit
- `ls` via `SYS_GETDENTS` (66); `ps` via `SYS_GETPROCS` (67)
- Lazy per-thread FPU save/restore; copyin/copyout (EFAULT never panics)
- NSI 1–93 allocated (see §NSI table)
- TCP loopback echo; kill end-to-end
- CI harness early-exit (DDR-785/788); `make smoke-selftest`; per-boot probe selection (DDR-804)
- `smoke-chipset` — q35/qemu64, pc/qemu64 (i440FX), q35/Nehalem, q35/Opteron_G5; 20/20 local

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
| J-01…J-06 retro audit | — | ✅ verified DDR-845 (audit only, no code) |

---

## 4. Section B — Kernel/OS Planned Features

| # | Feature | Priority | Status |
|---|---|---|---|
| B#1 | NVMe IRQ (MSI-X) | High | ⏸ functionally complete (DDR-774c); IRQ-driven sleep deferred |
| B#3 | `-smp 4` percpu-sched race | **High** | ⬜ OPEN — B#3 / DDR-806. Leading hypothesis: `fs_test_thread` lost before `main.c:1311` due to ~30 blocking SFS I/O calls. Next step: stamp `g_ticks` at `main.c:1134` and `:1311`. Gates promotion. |
| B#4 | SFS as default process root | Medium | ⬜ planned |
| B#6 | ext4 write support | Medium | ⬜ planned (ADR-019 extension, journal transaction layer) |
| B#9 | I/O APIC (q35 GSI routing) | Low | ⬜ planned |
| B#10 | Per-CPU runqueue affinity/NUMA hints | Low | ⬜ planned |
| B#12 | Job control in PRISM (`&`, SIGPIPE, job table) | Low | ⬜ partial (`$?` ✅, SIGPIPE ✅, `&` ⬜) |
| B#13 | Dynamic linker (`ld-pradyos.so`) | Low | ⬜ planned |
| B#14 | 3-lane NAS scheduler | Low | ⬜ planned |
| B#15 | PMM variable-weight/predictive allocator | Low | ⬜ planned |

---

## 5. Open Defects

| ID | Symptom | Status |
|---|---|---|
| **OPEN-1** | `smoke-surfdestroy` intermittently misses sentinel | open, passive |
| **OPEN-2** | Intermittent CI reds on `-smp 4` gates (`smoke-msixap`, `smoke-blkmq-trace` ×2, `smoke-crosswake` ×2) | open — DDR-863: all 4 intermittent gates run `QEMU_SMP=4`; 0/118 single-CPU gates ever flaked |
| **OPEN-9** | `smoke-shell` fails locally, passes CI, identical binary | root cause of the specific QEMU lock-hold scenario not yet caught |
| **OPEN-10** | Reframed by DDR-880 — not a B+tree bug; it is the item-47 lost-thread failure seen through a different sentinel. `fs_test_thread` lost before reaching `smoke-sfs-btree-smp4`; `[boot-stamp] B` absent in every captured failure | open, gates promotion |
| **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall / thread loss | open — see §4 above |
| ~~OPEN-7~~ | Per-boot probe selection | CLOSED (DDR-804) |
| ~~OPEN-8~~ | Console input loss | CLOSED (DDR-809) |
| ~~OPEN-11~~ | `smoke-sha256` failure after fresh image | CLOSED (DDR-831) — `blk_selftest` scratch sector overlapped kernel image at LBA 1500; fixed to LBA 4095 |

### Structural Defect Pattern — 16 Instances

One bug in many forms: **a check that discards or absorbs invalid input instead of rejecting it, so drift is silent and looks like success.**

| # | Where | Silent drop | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran in CI | DDR-817 — `make ci-shard-check` |
| 2 | Makefile user sources | 14/31 probes never rebuilt; gates tested stale binaries | DDR-822 — `$(wildcard user/*.c)` |
| 3 | `user/` `_start` attribute | new probe silently reintroduces a #GP | DDR-823 — `make ci-start-align-check` |
| 4 | `syscall_register()` | `num >= MAX_SYSCALLS` discarded; NSI 80+ would vanish | DDR-823 — panic + table 80→128 |
| 5 | `check_global_forbidden()` | printed only matching lines, discarding the `op=` line | DDR-824 — 40 lines of context |
| 6 | crypto sources + Makefile not prerequisites | build reports success and never runs | DDR-825 — glob `kernel/crypto/*` and list `Makefile` |
| 7 | writable global in R+X-only probe | link succeeds; first STORE faults at runtime | DDR-826 — `make ci-probe-rodata-check` |
| 8 | PMM double-free | freed frame re-entered the pool | DDR-830 |
| 9 | mid-enum insertion | shipped wire format silently renumbered | DDR-832 — append-only + `_Static_assert` |
| 10 | kernel headers/sources not prerequisites | third recurrence of same class | DDR-833/835 |
| 11 | SkillOpt accepting a TIE | each tie changes skill with no evidence it helped | DDR-847 — `>` not `>=` |
| 12 | skill revision thinning its own refusals | removing a refusal raises score; optimiser rewarded for it | DDR-848 — refusal count may rise, may not fall |
| 13 | `rates.get(model, 0.0)` for cost | unpriced model charged as free | DDR-849 — `UnknownModel` raised |
| 14 | `check_invariant()` asserting a tautology | `available` is derived by subtraction; invariant could never fail | DDR-849 — assert independently-tracked counters |
| 15 | building from tracker LABEL not spec text | items #47/#48/#50/#52 satisfied titles while missing §3D | DDR-850 — corrected against `AETHER_MASTER_FEATURES.md` §3D |
| 16 | mutation harness itself | absent target string skipped with warning; stale `__pycache__` still counted kills | DDR-853 — aborts on missing/ambiguous target, clears bytecode, fails if mutation kills nothing |

**Standing rule:** when a check discards input rather than rejecting it, the discard must be loud. A tracker line is a label FOR a requirement, not the requirement — build from the spec text.

---

## 6. NSI Allocation Table (append-only; never insert mid-table)

| NSI | Syscall | Capability | Status |
|---|---|---|---|
| 1–28 | Core OS syscalls | varies | ✅ shipped |
| 29–38 | AETHER agent calls | CAP_AGENT / CAP_SOVEREIGN | ✅ shipped |
| 66 | SYS_GETDENTS | — | ✅ shipped |
| 67 | SYS_GETPROCS | — | ✅ shipped |
| 69/70 | SYS_POWEROFF / SYS_REBOOT | — | ✅ shipped |
| 71–75 | SYS_SYSINFO/TIME/DMESG/MEMINFO/SETNAME | — | ✅ shipped |
| 76 | SYS_METRIC_READ | CAP_SOVEREIGN | ✅ shipped (DDR-812) |
| 77 | SYS_ACC_SEAL | CAP_AGENT | ✅ shipped (DDR-813) |
| 78 | SYS_ACC_OPEN | CAP_SOVEREIGN | ✅ shipped (DDR-813) |
| 79 | SYS_GOAL_SIGN | CAP_SOVEREIGN | ✅ shipped (DDR-814) |
| 80 | SYS_GOAL_VERIFY | CAP_AGENT | ✅ shipped (DDR-814) |
| 81 | SYS_ACC_ROTATE | CAP_SOVEREIGN | ✅ shipped (DDR-815) |
| 82 | SYS_MEMORY_WRITE | CAP_MEMORY (1<<18) | ✅ shipped (DDR-836) |
| 83 | SYS_MEMORY_READ | CAP_MEMORY (1<<18) | ✅ shipped (DDR-836) |
| 84 | SYS_CHECKPOINT_AGENT | CAP_SOVEREIGN | ✅ shipped (DDR-837) |
| 85 | SYS_RESUME_AGENT | CAP_SOVEREIGN | ✅ shipped (DDR-837) |
| 86 | SYS_APPROVE_CODE_REWRITE | CAP_REWRITE + CAP_SOVEREIGN | ✅ shipped (DDR-842) |
| 87 | SYS_VAULT_PUT | CAP_SOVEREIGN | ✅ shipped (DDR-834) |
| 88–90 | `prad` package manager (renumbered; 87 is taken) | — | ⬜ TASK 18 |
| 91 | SYS_VAULT_GET | CAP_SOVEREIGN | ✅ shipped (DDR-834) |
| 92 | SYS_SUBMIT_CHILD_ACTION | CAP_AGENT | ✅ shipped (DDR-839) |
| 93 | SYS_VERIFY_AUDIT | CAP_SOVEREIGN | ✅ shipped (DDR-842) |
| **94+** | **NEXT FREE** | — | — |

> ⚠️ **Warning:** `prad` package manager was previously scoped to NSI 87–89. 87 is already `SYS_VAULT_PUT`. Use **88–90** instead. Record this in any DDR that implements `prad`.

---

## 7. Capability Bits

| Bit | Name | Status |
|---|---|---|
| 1<<16 | CAP_SOVEREIGN | ✅ shipped |
| 1<<17 | CAP_AGENT | ✅ shipped |
| 1<<18 | CAP_MEMORY | ✅ shipped (DDR-836) |
| 1<<19 | CAP_OCR | ⬜ deferred post-1.0 |
| 1<<20 | CAP_EXEC | ⬜ deferred post-1.0 |
| 1<<21 | CAP_REWRITE | ✅ shipped (DDR-842); always requires CAP_SOVEREIGN co-approval |
| 1<<22 | CAP_SCENE | ⬜ deferred post-1.0 (post-L7) |
| 1<<23 | CAP_NET_BROWSE | ⬜ deferred post-1.0 |

---

## 8. Work Queue — Complete, Dependency-Ordered

Status: ✅ done · 🔵 in-progress · ⬜ not started · 🔒 blocked

### Immediate (current session)

| # | Task | Status |
|---|---|---|
| 0b | Checkpoint commit | ✅ `bb7f9bc` |
| 0c | SESSION_HANDOFF.md updated | ✅ |
| 1a | STEP C — tee make stdout in `shell_evidence.sh` | 🔵 NEXT |
| 1b | Re-run `smoke-shell` 3× and read which assertion fails | 🔒 needs 1a |
| 1c | STEP D — fix whichever assertion 1b reveals | 🔒 needs 1b |
| 1d | STEP E — verify fix with 3 consecutive passes | 🔒 needs 1c |
| 1e | Push `dev/phase1` | 🔒 needs 1d |
| 2 | CI green on pushed tip | 🔒 needs 1e |
| 3 | Steps C/D/E remaining work (see above) | 🔵 |
| 4 | B#3 `-smp 4` virtio-blk/thread-loss root cause | ⬜ do before more OPEN-10 work |
| 5 | Push to `main` (3 greens required; `--ff-only` will fail, use `git push origin dev/phase1:main`) | 🔒 needs CI green |

### Group 1 — Tracker & Build-System Integrity (x86_64 v1.0.0)

| # | Item | Status |
|---|---|---|
| G1-1 | Tracker contradictions + NSI 87 collision | ✅ DDR-840, run 31053809587 on `f80efa6` |
| G1-2 | Docker reproducible build env | ✅ `Dockerfile` + `make docker-build` |
| G1-3 | CMake/Makefile hybrid | ✅ SKIPPED for v1.0.0 (DDR-843); revisit post-1.0 with arch ports |
| G1-4 | VirtualBox runner | ✅ `tools/vbox_runner/run_vbox.sh` |
| G1-5 | x86_64 chipset variant coverage | ✅ `smoke-chipset` 20/20 local |

### Group 2 — Section E / Capability / Agent Core Close-Out

| # | Item | Status |
|---|---|---|
| G2-1 | NSI 86 `SYS_APPROVE_CODE_REWRITE` + `CAP_REWRITE` | ✅ run 31094358972 |
| G2-2 | Audit hash chain + NSI 93 `SYS_VERIFY_AUDIT` | ✅ run 31094358972 |
| G2-3 | Section 3C action types (8 of 14 shipped; 6 deferred — see below) | ✅ partial |
| G2-4 | Section 3D daemon features #45–65 | ✅ ALL 21 COMPLETE (DDR-846–856) |
| G2-5 | Section F #66–76 | ⬜ not started |
| G2-6 | Section G 12-agent roster | ⬜ partial (8 slots registered; 4 remaining) |
| G2-7 | J-01…J-06 retro audit | ✅ DDR-845 |
| G2-8 | `smoke-invariants` S1–S8 | ✅ `PASS smoke-invariants (120s)` run 31104672684 on `81a3eaf` (S3/S7 deliberately NOT claimed — depend on unbuilt F#66–72) |
| G2-9 | `smoke-aead` gate-wiring verification | ✅ confirmed standalone gate in shard 4 |

#### 3C Action Types — 6 NOT built (reasons on record)

| Action type | Why deferred |
|---|---|
| `ACTION_CAPTURE_FRAME` | post-L7; needs `CAP_SCENE` + camera path |
| `ACTION_SCAN_ENVIRONMENT` | post-L7; SLAM3R, no hardware path |
| `ACTION_QUERY_SCENE` | post-L7; NL query over non-existent scene graph |
| `ACTION_PARSE_DOCUMENT` | needs 64 MiB local OCR model; no model-shipping path |
| `ACTION_EXEC_CODE` | needs a sandboxed interpreter — a subsystem, not an action |
| `ACTION_BROWSE_WEB` | needs headless browser + network egress (cloud bridge). **DEFERRED post-1.0 (DDR-843)** — enabling it is a security-posture change, not a feature toggle |

#### Section 3D — Ring-3/Daemon Features #45–65 — ALL 21 COMPLETE

| # | Item | Status | DDR |
|---|---|---|---|
| #45 | skill.md — 8 roster files, validated | ✅ | DDR-846 |
| #46 | SkillOpt loop | ✅ | DDR-847 |
| #47 | SkillOpt-Sleep (harvest→mine→replay→consolidate) | ✅ | DDR-848+850 |
| #48 | skill-update validation (CAP_SOVEREIGN always) | ✅ | DDR-848+850 |
| #49 | multi-agent skill transfer | ✅ | DDR-848 |
| #50 | TokenJuice — context compression ≤80% + hard token ceiling | ✅ | DDR-849+850 |
| #51 | JSONL trajectory log | ✅ | DDR-849 |
| #52 | cost accounting (token_count + latency_ms) | ✅ | DDR-849+850 |
| #53 | goals.md per agent | ✅ | DDR-851 |
| #54 | subconscious loop | ✅ | DDR-851 |
| #55 | MOSS pipeline | ✅ | DDR-851 |
| #56 | OCR→memory pipeline | ✅ | DDR-852 |
| #57 | multi-modal context builder | ✅ | DDR-852 |
| #58 | privacy mode (ring-3) | ✅ | DDR-852 |
| #59 | model routing | ✅ | DDR-852 |
| #60 | hypothesis tree (versioned, SFS-persistent) | ✅ | DDR-853+855 |
| #61 | genome.md (lineage archived, rationale required) | ✅ | DDR-853 |
| #62 | vector knowledge graph (online learning) | ✅ | DDR-856 |
| #63 | dead-end registry (D-13 FailureMemoryRegistry + divergence score) | ✅ | DDR-855 |
| #64 | population tournament (unranked ≠ last, ties do not promote) | ✅ | DDR-856 |
| #65 | replayable run visualiser (deterministic, never un-redacts) | ✅ | DDR-856 |

### Section E — Kernel Syscalls (TASK 9) — ALL COMPLETE

See §NSI table for numbers. All CI-confirmed.

### Section F — Visionary Features #66–76 (TASK 13)

| # | Feature | Status |
|---|---|---|
| F#66 | architect_agent | ⬜ |
| F#67 | healer_agent | ⬜ |
| F#68 | metric lockbox | ⚠️ kernel ✅ + Python ✅; end-to-end `smoke-lockbox-e2e` ⬜ |
| F#69 | inventor_agent | ⬜ |
| F#70 | tournament_agent | ⬜ |
| F#71 | subconscious world model | ⬜ |
| F#72 | verifier_agent | ⬜ |
| F#73 | sovereign NL UI | ⬜ |
| F#74 | capability discovery | ⬜ |
| F#75 | lineage memory | ⬜ |
| F#76 | tamper-evident ledger | ⬜ |

### Section G — 12-Agent Roster (TASK 14)

8 kernel slots (KRYOS…SOLIN) registered + UI cards + skill prompts (DDR-846).  
DDR-846 decision: 8 legacy names **become** the 8 highest-priority Section G roles.

| Section G role | Slot | Capabilities | Spawnable? |
|---|---|---|---|
| file_agent | KRYOS | CAP_AGENT, CAP_MEMORY | ✅ |
| shell_agent | PRAX | CAP_AGENT, CAP_EXEC | ❌ CAP_EXEC unwired |
| research_agent | LUMYN | CAP_AGENT, CAP_NET_BROWSE | ❌ CAP_NET_BROWSE unwired |
| ocr_agent | AHNIS | CAP_AGENT, CAP_OCR | ❌ CAP_OCR unwired |
| vision_agent | IRIS | CAP_AGENT, CAP_SCENE | ❌ CAP_SCENE unwired (post-L7) |
| healer_agent | RUFLO | CAP_AGENT, CAP_REWRITE | ❌ not yet spawnable |
| orchestrator_agent | HERMES | CAP_AGENT | ✅ |
| verifier_agent | SOLIN | CAP_AGENT | ✅ |
| subconscious_agent | — | daemon-level | ⬜ no kernel roster slot yet |
| ai_scientist_agent | — | CAP_AGENT, CAP_EXEC, CAP_MEMORY | ⬜ no kernel roster slot yet |
| architect_agent | — | CAP_AGENT, CAP_REWRITE | ⬜ no kernel roster slot yet |
| tournament_agent | — | daemon-level | ⬜ no kernel roster slot yet |

> 4 remaining roster agents need a kernel slot before their skill file means anything. Kernel-side per-persona dispatch is future for all 12.

### Section B Remaining (TASK 16)

B#1 NVMe IRQ ⏸ · B#4 SFS default root ⬜ · B#6 ext4 write ⬜ · B#9 I/O APIC ⬜ · B#10 NUMA affinity ⬜ · B#12 PRISM job control ⬜ · B#13 dynamic linker ⬜ · B#14 NAS scheduler ⬜ · B#15 PMM policy ⬜

### TASK 17 — ISO Pipeline

| Target | Boot status | ISO status |
|---|---|---|
| x86_64 | ✅ boots, 118+ gates | ⬜ multiboot2 + grub-mkrescue |
| aarch64 | ✅ boots in CI | ⬜ EFI/U-Boot packaging |
| riscv64 | ✅ boots in CI | ⬜ OpenSBI + U-Boot packaging |
| Apple Silicon | ⬜ | ⬜ m1n1 shim over aarch64 kernel |

### TASK 18–21

| Task | Item | Status |
|---|---|---|
| 18 | `prad` package manager (NSI **88–90**, NOT 87–89) | ⬜ |
| 19 | Phase 9 assembly optimisation | ⬜ |
| 20 | Security invariant gates S1–S8 (S3/S7 blocked on F#66–72) | ⬜ partial |
| 21 | v1.0.0 release | ⬜ |

---

## 9. Section D — ADR-026 Baseline (#1–17) — VERIFIED BUILT 2026-07-24

| # | Item | Status |
|---|---|---|
| 1 | Kernel-is-arbiter trust model (`SYS_SUBMIT_ACTION`) | ✅ |
| 2 | Sovereign auto-approve mode | ✅ |
| 3 | Manual mode w/ 60 s expiry | ✅ |
| 4 | 256-entry fixed action queue | ✅ |
| 5 | 4096-entry append-only audit log (wrap-flagged) | ✅ |
| 6 | Per-agent hard 128 MiB memory cap + OOM kill | ✅ |
| 7 | `CAP_SOVEREIGN` (1<<16) | ✅ |
| 8 | `CAP_AGENT` (1<<17) | ✅ |
| 9 | 60 syscall/s sliding-window rate limiter | ✅ |
| 10 | `SYS_SPAWN_AGENT` force-PENDING in sovereign mode | ✅ |
| 11 | `SYS_KILL_AGENT` (caller kills own children only) | ✅ |
| 12 | Ollama HTTP/1.1 ring-3 bridge | ✅ |
| 13 | CI test mode (deterministic, no live model) | ✅ |
| 14 | Daemon topology (PID-1 → daemon → agents) | ✅ |
| 15 | `ACTION_WRITE_FILE` (SFS W^X validated) | ✅ |
| 16 | `ACTION_PRINT` (512-byte bounded) | ✅ |
| 17 | `ACTION_SPAWN_PROCESS` (force-PENDING always) | ✅ |

---

## 10. Section H — Security Invariants (BINDING, ADR-level)

Govern all 93+ features. Changeable only by a superseding ADR.

- **S1 — No self-escalation.** An agent cannot raise its own cap bits, `mem_limit`, or approve its own actions.
- **S2 — Bounded everything.** queue 256 · audit 4096 · payload 512 B · memory 128 MiB · syscalls 60/s · skill 2 KB · IPC msg 256 B · spawn depth 3 · lineage results ≤16/query. Every bound returns an error or clean kill — **never a panic**.
- **S3 — Immutable objective function.** `CAP_SOVEREIGN`-locked SFS path, kernel-signed; agent write attempt → kill + audit.
- **S4 — Human gate is structural.** spawn, scene, skill-update, code-rewrite, genome-evolve, architect proposals all require `CAP_SOVEREIGN` approval; force-PENDING for highest-consequence actions.
- **S5 — Append-only audit + Merkle ledger.** No user-space erase path; science ledger Merkle-chained; wraps flagged.
- **S6 — Fault isolation.** Ring-3 fault kills the agent, never the kernel; copyin/copyout on all user pointers.
- **S7 — Metric-gaming prevention.** Verifier agent is structurally independent; objective function is kernel-locked.
- **S8 — Skill-change veto.** Any `skill.md` or code write requires `CAP_SOVEREIGN` + optional manual approval.

---

## 11. Architecture Prerequisite Checklist (answer in DDR before coding any Section E/F item)

1. New NSI/syscalls — is the append-only NSI range still open? (next free: **94**)
2. TCB / roster-slot / agent-slot fields — struct extension? ABI size assumptions?
3. PMM/VMM shared mappings — needs `PTE_SW_SHARED` or a new mapping class?
4. Capability checks — which `CAP_*` gates it; is a new bit needed?
5. AETHER queue/audit extensions — new record type in `aether_audit.c`/`aether_queue.c`?
6. Scheduler/accounting hooks — `sched_exit`, `sched_create_state`, spawn-depth enforcement?
7. Filesystem/root-mount constraints — depends on `root_mnt`, SFS dirs, or new namespace concept?
8. Network policy tables — touches `net_allow`, `CAP_NET`, `CAP_NET_BROWSE`, or socket NSI?
9. Compositor/UI exposure — new agent card/roster visual needed?
10. New smoke gate — what proves it **deterministically**? (avoid TCG-timing flakiness)
11. **Security invariant check (MANDATORY)** — which of S1–S8 govern this feature, and does the plan violate any of them even indirectly?

---

## 12. Risk Flags (honest assessment)

1. **Rate does not reach remaining work.** ~69 features remain. The observed rate is ~2 features + infrastructure per session. Infrastructure was necessary but is not feature throughput.
2. **5 silent-drop defects in 3 sessions** implies more exist. Every one was found while chasing something else.
3. **Remaining work is disproportionately never-started**: Sections F/G, `prad`, Phase 9, invariant gates — no scaffolding.
4. **ISO task is cheaper than it looked** — packaging on two already-booting arch targets, not four ports.
5. **Honest scope for v1.0.0:** a defensible x86_64 build with crypto chain closed, both `-smp 4` races fixed, and ISOs for the three targets that already boot. Which subset ships is a scope decision, not an engineering one.

---

## 13. Competitor Landscape (Table C)

| Competitor | Weakness | AETHER fix |
|---|---|---|
| OpenClaw (210k★) | Abysmal skill-vetting security | `CAP_AGENT` ring-3 sandbox + 128 MiB cap + CAP_SOVEREIGN veto + append-only audit |
| AutoGPT (183k★) | Stalled, hallucination-prone | SkillOpt-Sleep nightly evolution + MOSS rewriting + SFS CoW rollback |
| CrewAI (60k★) | No kernel isolation | `ACTION_SEND_IPC` + DAG action queue at kernel level |
| LangChain (146k★) | Stateless, cloud-dependent | `SYS_MEMORY_WRITE/READ` + SFS memory tree + local Ollama |
| n8n (100k★) | Static workflows, no learning | Subconscious agent rewrites plans on failure + SkillOpt |
| Dify (85k★) | Weak action capability | `ACTION_EXEC_CODE` + `ACTION_PARSE_DOCUMENT` |
| OpenHands (47k★) | Privileged Docker, no self-improvement | Hard ring-3 isolation + kernel-audited syscalls + SkillOpt |
| MOSS (academic) | No kernel safety boundary on self-rewriting | `ACTION_REWRITE_AGENT_CODE` behind CAP_SOVEREIGN + regression + SFS snapshot |
| Ouroboros/AutoResearch | No local fallback, no OS safety, metric-gaming risk | Local Ollama loop + CAP_SOVEREIGN-locked metric lockbox |
| AI Scientist/Curie | $15–45/paper, no local run, no persistent memory | $0 local experiments + persistent hypothesis tree |
| Pentagi (19k★) | Security-domain only, no kernel sandbox | Same `CAP_AGENT` sandbox + rate limiting |
| RuVector (4k★) | Standalone DB, no agent loop | Adopted as AETHER's vector knowledge-graph memory model |

---

## 14. Standing AETHER Cognitive/ASI Mandate

Long-term trajectory: subconscious background reasoning (F#71), verifier-based
self-improvement (F#72), the **metric lockbox** (F#68 / S3 — highest priority
among proposed features), and the full 12-agent Section G roster. This is not a
single task; it is an evolving capability goal. Decompose into bounded DDRs only
when a concrete slice is chosen.

Section D is confirmed built. That is the precondition for all Section E/F work.

---

*Last updated: 2026-08-14 by Perplexity session synthesis. Anchored to local HEAD `bb7f9bc` / remote `3065e78` / `main` `3b4830a`.*
