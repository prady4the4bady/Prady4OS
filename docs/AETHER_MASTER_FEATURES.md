# PradyOS AETHER — Master Feature Tables

**Canonical, single source of truth for feature state.** No competing feature list
may exist anywhere in the repo. Mirrored into `SESSION_HANDOFF.md` (task tracker)
and `docs/build_status.md` (gate count/header) in the SAME commit as any code
touching agents, UI, sockets, storage, namespaces, telemetry, scheduling, or
capabilities.

**Status vocabulary:** `shipped` (CI-green today) · `active-slice` (a DDR/ADR is
open and code is in flight) · `planned` (tracked, not started) · `proposed`
(designed, prerequisites not yet answered).

**Last verified against repo:** 2026-07-24, `main` @ `3485085` (DDR-773),
106 gates green (CI run 30132610321).

---

## Section A — Kernel/OS shipped features (CI-green; do not re-litigate)

All entries below are **shipped**.

### Boot / Kernel / Drivers / Filesystem
- MBR two-stage boot → long mode → ring-0 C (gate: `smoke`)
- Kernel relocated to 4 MiB, 768 KiB load window (DDR-733)
- GDT/IDT + exception panic path; 8259 PIC ISA-only; LAPIC/APIC timer @100 Hz (DDR-714A)
- Buddy PMM, SLAB heap, higher-half VMM, per-process CR3, W^X + NX (ADR-003/007/021)
- Per-CPU runqueues + work-stealing scheduler (DDR-SMP-rq-1/2/3); SMP bring-up 4 APs (ADR-029/031)
- MSI-X for all virtio devices (DDR-714C, DDR-771); multi-in-flight virtio-blk (DDR-BLK-1)
- NCS capabilities: `CAP_SOVEREIGN`, `CAP_AGENT`, `CAP_NET` (ADR-009, DDR-731)
- NIA IPC sync/async/broadcast (ADR-010/011)
- FAT32 RW + VFAT LFN (ADR-015/020); SFS CoW B+tree, extents, journal, LZ4, snapshots (ADR-018)
- SFS hierarchical dirs (DDR-738), unlink/rmdir (DDR-741), free-space GC (DDR-762-v2)
- SFS write-budget token bucket 25 MiB/s (ADR-032); cross-reboot persistence (DDR-768/769/770)
- Host `mkfs.sfs` tool (DDR-767) + multi-leaf B+tree bulk load (DDR-773); ext4 read-only (ADR-019); per-process root mount (DDR-739)
- NVMe controller + block I/O (DDR-765/766); NVMe PRP2/PRP-list (DDR-772); `VBLK_MAX` 4→8 (DDR-771)

### Userspace / Syscalls / Shell
- Static ELF64 loader + per-process W^X AS (ADR-021); musl libc v1.2.5 (ADR-023)
- `pradyos-init` PID 1 + orphan reaper; PRISM shell w/ full-register fork (ADR-024)
- PRISM builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit
  — `ls` enumerates via `SYS_GETDENTS` (DDR-742) and `ps` via `SYS_GETPROCS` (DDR-743) (was mis-tracked as planned in Section B#8 until 2026-07-24)
- Lazy per-thread FPU save/restore, user-only (DDR-740); copyin/copyout (EFAULT never panics)
- NSI 1–75 shipped; `SYS_GETDENTS` (66), `SYS_GETPROCS`, `SYS_POWEROFF`/`REBOOT` (69/70)
- `SYS_SYSINFO`/`TIME`/`DMESG`/`MEMINFO` (71–74), `SYS_SETNAME` (75), TCP loopback echo, kill end-to-end

### AETHER agent layer (kernel plumbing)
- Kernel action queue + append-only audit log (ADR-026); per-process mem cap + syscall rate limit
- 10 NSI agent calls (29–38); `CAP_SOVEREIGN`/`CAP_AGENT`; ring-3 daemon + `agent_base.c`
- Ring-3 socket NSI, 8 proxy sockets, live Ollama over HTTP (ADR-027)
- AETHER boot config from `/etc/aether/config` via SFS (DDR-732, DDR-770)
- `CAP_NET` allowlist, deny-by-default egress (DDR-734); agent CPU metrics (DDR-735/736)
- Per-agent live metrics, post-mortem stable (DDR-730); `SYS_AGENT_ROSTER` 8 named slots (DDR-707)
- 8 named agents KRYOS…SOLIN with UI panel cards + action pips (DDR-737)

### Layer 7 UI / Sovereign desktop
- VirtIO-GPU framebuffer (ADR-028); ring-3 FB surface `SYS_FB_INFO/MAP/FLUSH` (DDR-702)
- PS/2 keyboard `SYS_INPUT_POLL` (DDR-703); virtio-input pointer `SYS_MOUSE_POLL` (DDR-705)
- Sovereign/Manual mode toggle (DDR-701); compositor w/ 8×8 font (DDR-704)
- Per-client surfaces `SYS_SURFACE_*` (DDR-706); z-order/focus/key routing (DDR-708)
- Sun-driven OKLab ambiances (DDR-709); window drag/close/resize/minimize/maximize (DDR-710/711/717/719)
- Glass blur, gradients, particle field, decorations, alt-tab, page flip, scroll, spring/ripple, Inter font
  (DDR-712/720/721/722/723/724/725/726/727/728)
- Surface destroy lifecycle safety (DDR-729); agent-card click → `SYS_SPAWN_AGENT` (DDR-713)

---

## Section B — Kernel/OS planned features (tracked)

| # | Feature | Priority | Status | Requires |
|---|---|---|---|---|
| 1 | NVMe IRQ (MSI-X vector) | High | **re-scoped → DDR-774a/b/c** (blast-radius reviewed 2026-07-24) | **Vector availability was never the blocker** (DDR-771 vacated 50–53). The real cost is three coupled surfaces: (a) no generic PCI MSI-X programmer exists — the only one is virtio-coupled (`virtio_pci_msix_setup` takes `struct virtio_pci_dev*`, uses virtio's `map_bar` + `common_cfg.queue_msix_vector`), so it must be refactored out of a path serving blk/net/gpu/input; (b) `nvme.c` maps a fixed 2-page (`0x2000`) BAR0 window, but the MSI-X table offset/BIR must be read at runtime and may lie outside it or in another BAR; (c) completion moves from thread-context poll to IRQ context. Split into **774a** generic MSI-X helper (pure refactor, existing gates) — **DONE**, see `docs/ddr/DDR-774a-generic-pcie-msix.md`: `pcie_msix_find`/`pcie_msix_program`/`pcie_intx_disable` now live in `pcie.c`, `virtio_pci_msix_setup` reimplemented on them with identical signature/register order (all 3 virtio callers unchanged) — then **774b** NVMe table mapping + `IEN` — **DONE but with an OPEN ISSUE**: the table is found/mapped/programmed and `smoke-nvme` stays green (`[nvme] msix vec=50`), yet **`[nvme] irqs=0` — no interrupt is actually delivered**. Ruled out: interrupts-masked (`sti` at main.c:1478/1737 precedes `nvme_init` at :1801). Remaining suspects: MSI-X Function-Mask bit (MC bit 14) never explicitly cleared, table address math, per-entry vector control. See `docs/ddr/DDR-774b-nvme-msix-table.md`. **774c must root-cause delivery FIRST**, before converting the completion path, then add the bounded spin fallback (S2) + a count-based sentinel. See `docs/ddr/DDR-774-nvme-irq-scoping.md`. |
| 2 | mkfs.sfs multi-leaf B+tree (>14 slots) | High | **shipped (DDR-773)** — moved to Section A | Host tool mirrors kernel `sfs.c` descend/node format exactly |
| 3 | `-smp 4` percpu-sched race root-cause | High | planned | A narrow reproducer first; bounded `g_ticks` deadline poll replacing single-shot checks. CI symptom currently masked by the DDR-771 timeout bump (90→180 s / 60→120 s) — that is mitigation, **not** a root cause. |
| 4 | SFS as default process root | Medium | planned | Provisioned SFS image as default `root_mnt`; retire blk2's dual role (scratch + root). Unblocked by DDR-771. |
| 5 | COW fork | Medium | planned | Page-fault handler: mark RO on fork, clone on write fault, PMM refcount |
| 6 | ext4 write support | Medium | planned | ADR-019 extension, journal transaction layer |
| 7 | Kernel self W^X (kernel text RX / data NX) | Medium | planned | Split boot page tables into RX text + RW data |
| 8 | `ls`/`ps` full implementations | — | **shipped — entry was STALE, corrected 2026-07-24; moved to Section A** | Drift found while scoping: both are implemented in `user/prism.c` — `ls` over `SYS_GETDENTS` (66, DDR-742) and `ps` over `SYS_GETPROCS` (67, DDR-743). The "requires a process-table scan syscall" precondition was already satisfied. |
| 9 | I/O APIC (q35 GSI routing) | Low | planned | Would replace 8259 for ISA devices |
| 10 | Per-CPU runqueue affinity/NUMA hints | Low | planned | CPU topology hints extension to DDR-SMP-rq-1 |
| 11 | wlroots/Wayland compatibility | Low | planned | EGL/GBM bridge to VirtIO-GPU; blocked by no-out-of-tree-libs policy |
| 12 | Pipes/redirection/job control in PRISM | Low | planned | `SYS_PIPE`, FD dup2, SIGPIPE, background job table |
| 13 | Dynamic linker (`ld-pradyos.so`) | Low | planned | Relocatable ELF loader, GOT/PLT patching |
| 14 | 3-lane NAS scheduler | Low | planned | Lane classification field in TCB |
| 15 | PMM variable-weight/predictive allocator | Low | planned | Policy-driven allocator over current buddy |

---

## Section C — Competitor landscape (Table 1)

What to beat, and how AETHER structurally fixes it.

| Competitor | Scale | Structural weakness | AETHER fix |
|---|---|---|---|
| OpenClaw | 210k+ ★ | Abysmal skill-vetting security | `CAP_AGENT` ring-3 sandbox + 128 MiB cap + no self-escalation + `CAP_SOVEREIGN` veto + append-only audit |
| AutoGPT | 183k ★ | Stalled, hallucination-prone | SkillOpt-Sleep nightly evolution + MOSS-style rewriting + SFS CoW rollback |
| CrewAI | 60k+ ★ | No kernel isolation, logic-flow only | `ACTION_SEND_IPC` + DAG action queue at kernel level |
| LangChain/Langflow | 146k ★ | Stateless, cloud-dependent | `SYS_MEMORY_WRITE/READ` + SFS memory tree + local Ollama |
| n8n | 100k+ ★ | Static workflows, no learning | Subconscious agent rewrites plans on failure + SkillOpt validation |
| Dify | 85k+ ★ | Weak action capability | `ACTION_EXEC_CODE` + `ACTION_PARSE_DOCUMENT` |
| OpenHands/OpenDevin | 47k ★ | Privileged Docker, no self-improvement | Hard ring-3 isolation + kernel-audited syscalls + SkillOpt |
| MOSS | academic | No kernel safety boundary on self-rewriting | `ACTION_REWRITE_AGENT_CODE` gated behind `CAP_SOVEREIGN` + mandatory regression pass + SFS snapshot |
| Ouroboros/AutoResearch | Karpathy | No local fallback, no OS safety, metric-gaming risk | Local Ollama loop + `CAP_SOVEREIGN`-locked metric lockbox |
| AI Scientist/Curie | Sakana | $15–45/paper, no local run, no persistent memory | $0 local experiments + persistent hypothesis tree |
| Pentagi | 19k ★ | Security-domain only, no kernel sandbox | Same `CAP_AGENT` sandbox + rate limiting |
| RuVector | 4k ★ | Standalone DB, no agent loop | Adopt as AETHER's vector knowledge-graph memory model |

---

## Section D — ADR-026 baseline (#1–17) — **VERIFIED BUILT 2026-07-24**

Re-verified against actual repo state this session (not assumed). Evidence:
`kernel/aether/{aether.c,aether.h,aether_audit.c,aether_mem.c,aether_queue.c}`,
`kernel/cap.h`.

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Kernel-is-arbiter trust model (`SYS_SUBMIT_ACTION` gate) | shipped | `SYS_SUBMIT_ACTION` present |
| 2 | Sovereign auto-approve mode | shipped | `sovereign` mode in `aether.c` |
| 3 | Manual mode w/ 60 s expiry (`SYS_APPROVE_ACTION`/`SYS_REJECT_ACTION`) | shipped | both syscalls present |
| 4 | 256-entry fixed action queue (no heap, DoS-resistant) | shipped | `AETHER_QUEUE_LEN 256` |
| 5 | 4096-entry append-only audit log (wrap-flagged) | shipped | `AETHER_AUDIT_LEN 4096` |
| 6 | Per-agent hard 128 MiB memory cap + OOM kill | shipped | `AETHER_MEM_DEFAULT (128ull << 20)` |
| 7 | `CAP_SOVEREIGN` bit (1<<16) | shipped | `cap.h:40` |
| 8 | `CAP_AGENT` bit (1<<17) | shipped | `cap.h:41` |
| 9 | 60 syscall/s sliding-window rate limiter | shipped | `AETHER_RATE_MAX 60`, `AETHER_RATE_WINDOW 100` ticks |
| 10 | `SYS_SPAWN_AGENT` force-PENDING even in sovereign mode | shipped | `SYS_SPAWN_AGENT` present |
| 11 | `SYS_KILL_AGENT` (caller kills own children only) | shipped | `SYS_KILL_AGENT` present |
| 12 | Ollama HTTP/1.1 ring-3 bridge (lwIP only, minimal JSON scanner) | shipped | ADR-027, Section A |
| 13 | CI test mode (deterministic, no live model) | shipped | gate suite is model-free |
| 14 | Daemon topology (PID-1 → daemon → agents) | shipped | DDR-761/770 |
| 15 | `ACTION_WRITE_FILE` (SFS W^X validated) | shipped | present |
| 16 | `ACTION_PRINT` (512-byte bounded) | shipped | `AETHER_PAYLOAD_MAX 512` |
| 17 | `ACTION_SPAWN_PROCESS` (force-PENDING always) | shipped | present |

**No drift found.** Next free capability bit is `1<<18`, matching Section E's
`CAP_MEMORY` plan. **Section D being confirmed built is the precondition for
starting any Section E/F work.**

---

## Section E — New features: kernel extensions, ring-3, post-L7 (#18–65)

All entries **proposed** until a DDR answers the architecture prerequisite
checklist for that specific item.

### 3A — New kernel syscalls (NSI appends start at **76**)
- `SYS_MEMORY_WRITE`/`SYS_MEMORY_READ` — persistent per-agent SFS CoW B+tree memory (`CAP_MEMORY` 1<<18)
- `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` — SFS snapshot serialize/restore, integrity-hashed, `CAP_SOVEREIGN`
- `spawn_depth` cap in TCB — child `mem_limit = parent/(depth+1)`, hard cap depth 3
- DAG action queue — `parent_action_id` u64 field
- `SYS_APPROVE_CODE_REWRITE` — `CAP_SOVEREIGN` gate for MOSS-style patches

### 3B — New capability bits
`CAP_MEMORY` (1<<18) · `CAP_OCR` (1<<19) · `CAP_EXEC` (1<<20) · `CAP_REWRITE`
(1<<21, **always** requires `CAP_SOVEREIGN` co-approval, never auto-granted) ·
`CAP_SCENE` (1<<22, post-L7 only) · `CAP_NET_BROWSE` (1<<23)

### 3C — New action types (#31–44)
`ACTION_READ_FILE` · `ACTION_DELETE_FILE` (force-PENDING in manual mode) ·
`ACTION_EXEC_CODE` (sandboxed interpreter, 32 MiB cap) · `ACTION_SEND_IPC`
(agent-to-agent via NAS IPC, peer-token gated) · `ACTION_PARSE_DOCUMENT`
(OCR→Markdown, 64 MiB cap, local model) · `ACTION_BROWSE_WEB` (headless browser) ·
`ACTION_QUERY_MEMORY` (semantic/graph search, read-only, ≤16 results) ·
`ACTION_CAPTURE_FRAME` (post-L7) · `ACTION_SCAN_ENVIRONMENT` (SLAM3R, post-L7,
first-use always manual-gate) · `ACTION_QUERY_SCENE` (post-L7 NL query) ·
`ACTION_REWRITE_AGENT_CODE` (MOSS pipeline, force-PENDING, `CAP_REWRITE` +
`CAP_SOVEREIGN` co-approval, regression suite must pass first) ·
`ACTION_PROPOSE_HYPOTHESIS` (logged to SFS hypothesis tree) · `ACTION_RUN_EXPERIMENT`
(`CAP_EXEC`; metric function lives in a `CAP_SOVEREIGN`-locked SFS path — **cannot**
be modified by the experimenting agent) · `ACTION_EVOLVE_GENOME` (force-PENDING,
rewrites `genome.md` across generations, `CAP_SOVEREIGN` veto window)

### 3D — Ring-3/daemon-only (#45–65, no kernel changes)
Per-agent `skill.md` (300–2000 tokens) · SkillOpt training loop
(rollout→reflect→aggregate→select→update→evaluate; accept only on strictly positive
held-out improvement) · SkillOpt-Sleep nightly evolution
(harvest→mine→replay→consolidate; pauses active agents) · skill-update validation
gate (`CAP_SOVEREIGN` always) · multi-agent skill specialization/transfer ·
TokenJuice context compression (≤80% tokens) · JSONL trajectory run journal (from
kernel audit log) · per-call cost accounting (`token_count` + `latency_ms` in audit
entries) · `goals.md` per agent · subconscious background loop (wakes every N ticks,
goal-diff prompts, bounded by the 60 syscall/s limiter) · MOSS source-rewriting
pipeline (staging sandbox, regression gate, SFS snapshot rollback) · OCR→memory
pipeline · multi-modal context builder · privacy mode (blocks non-local Ollama via
lwIP netfilter hook) · model routing · hypothesis tree in SFS (versioned, persists
across boots) · `genome.md` per research agent (lineage archived) · vector knowledge
graph memory (RuVector-style, online learning) · dead-end registry (failure reason +
divergence score, queried before repeating) · population tournament mode ·
replayable run visualizer (optional ring-3 web UI)

---

## Section F — Visionary ASI-trajectory features (#66–76)

All **proposed**. No competitor ships these.

| # | Feature | Note |
|---|---|---|
| 66 | Self-designing agent architecture | `architect_agent` proposes topologies/syscalls/cap bits, writes ADR, scaffolds C stubs + CI smoke test; single human approval |
| 67 | Self-healing rollback cascade | `healer_agent`: checkpoint→diagnose(MOSS)→patch→regression-verify→restore, fully kernel-audited |
| **68** | **IMMUTABLE OBJECTIVE FUNCTION / metric lockbox** | `CAP_SOVEREIGN`-read-only + kernel-signed SFS path; any `CAP_AGENT` write attempt → cap-escalation-denied audit + process kill. **THE single most important safety invariant for any self-improvement loop (= Invariant S3). HIGHEST PRIORITY among proposed features — every Section E/F self-improvement feature depends on it existing first.** |
| 69 | Autonomous software inventor | `inventor_agent`: hypothesis→experiment→build→test→commit, zero human-written code; human approves final commit only |
| 70 | Divergence-aware multi-lineage research | `tournament_agent` scores parallel genome lineages, rejects premature convergence, `CAP_SOVEREIGN` per generation |
| 71 | Sub-conscious world model | `subconscious_agent` maintains compressed world model (memory+scene+trajectory), fires proactive prompts to idle agents |
| 72 | Verifier-based RL reward signals | `verifier_agent` cross-checks every experiment with a **different** model before it enters the hypothesis tree or SkillOpt reward — structurally prevents metric gaming beyond the lockbox |
| 73 | Sovereign UI w/ natural-language OS control | ring-3/post-L7 panel: goals in plain English, sovereign/manual toggle, audit log, approve/reject, hypothesis tree + world model. No CLI, no prompt templates |
| 74 | Capability discovery from internet | `research_agent` browses GitHub, generates+tests+quarantines new skill wrappers pending `CAP_SOVEREIGN` approval |
| 75 | Lineage memory / ancestral knowledge | Departure summaries on clean agent exit only; new instances read all past lineage files before their first Ollama call — knowledge compounds across restarts |
| 76 | Tamper-evident science ledger | Merkle-chained SFS ledger; every hypothesis/experiment/result/reflection/genome-edit carries `prev_hash sha256[32]`; `SYS_READ_AUDIT` verifies chain on read; append-only |

---

## Section G — Named agent roster (12 agents; extends the shipped 8-slot KRYOS…SOLIN roster)

| Agent | Capabilities | Role | Source | Status |
|---|---|---|---|---|
| `file_agent` | CAP_AGENT, CAP_MEMORY | File R/W/delete; skill evolves via SkillOpt | ADR-026 | proposed |
| `shell_agent` | CAP_AGENT, CAP_EXEC | Code execution, shell automation, skill transfer | RLM | proposed |
| `research_agent` | CAP_AGENT, CAP_MEMORY, CAP_NET_BROWSE | Web browse, doc parse, capability discovery | OpenHuman+OCR | proposed |
| `ocr_agent` | CAP_AGENT, CAP_OCR | PDF/image → structured Markdown; feeds memory pipeline | Unlimited-OCR | proposed |
| `subconscious_agent` | daemon-level | World model, proactive goal-diff, SkillOpt-Sleep orchestration | OpenHuman | proposed |
| `ai_scientist_agent` | CAP_AGENT, CAP_EXEC, CAP_MEMORY | Hypothesis gen, experiments, genome evolution, inventor loop | AI Scientist/Ouroboros | proposed |
| `healer_agent` | CAP_AGENT, CAP_REWRITE | Crash detection, MOSS diagnosis, patch proposal, rollback | MOSS+ADR-026 | proposed |
| `architect_agent` | CAP_AGENT, CAP_REWRITE | Proposes topologies/action types, writes ADRs + C stubs | Self-designing | proposed |
| `verifier_agent` | CAP_AGENT | Independently cross-checks experiment results | Verifier-RL | proposed |
| `tournament_agent` | daemon-level | Scores competing lineages, selects genome survivor | Ouroboros | proposed |
| `orchestrator_agent` | CAP_AGENT, depth-0 spawn rights | Spawns/coordinates fleets, manages DAG queue | OpenHuman/RLM | proposed |
| `vision_agent` | CAP_AGENT, CAP_SCENE (post-L7) | FB capture, SLAM3R 3D reconstruction, scene queries | SLAM3R | proposed |

---

## Section H — Security invariants (BINDING, ADR-level)

These govern **all 76 features**. Binding exactly like ADR-021's W^X contract:
changeable only by a superseding ADR, never quietly amended, and checked against
every new Section E/F feature **before** implementation.

- **S1 — No self-escalation.** An agent cannot raise its own cap bits, `mem_limit`, or approve its own actions.
- **S2 — Bounded everything.** queue 256 · audit 4096 · payload 512 B · memory 128 MiB · syscalls 60/s · skill 2 KB · IPC msg 256 B · spawn depth 3 · lineage results ≤16/query. Every bound returns an error or a clean kill — **never a panic**.
- **S3 — Immutable objective function.** `CAP_SOVEREIGN`-locked SFS path, kernel-signed at build time; agent write attempt → kill + audit. (Covers #43, #66–72.)
- **S4 — Human gate is structural.** spawn, scene, skill-update, code-rewrite, genome-evolve, architect proposals all require `CAP_SOVEREIGN` approval; force-PENDING for the highest-consequence actions.
- **S5 — Append-only audit + Merkle ledger.** No user-space erase path; science ledger Merkle-chained; wraps flagged.
- **S6 — Fault isolation.** A ring-3 fault kills the agent, never the kernel; copyin/copyout on all user pointers.
- **S7 — Metric-gaming prevention.** The verifier agent is structurally independent; the objective function is kernel-locked.
- **S8 — Skill-change veto.** Any `skill.md` or code write requires `CAP_SOVEREIGN` + optional manual approval.

---

## Architecture prerequisite checklist (answer in the DDR before coding any Section E/F item)

1. New NSI/syscalls — is the append-only NSI range still open? (through 75; next append starts at **76**)
2. TCB / roster-slot / agent-slot fields — struct extension? ABI size assumptions?
3. PMM/VMM shared mappings — needs `PTE_SW_SHARED` (DDR-729 pattern) or a new mapping class?
4. Capability checks — which `CAP_*` gates it (existing or 1<<18..1<<23); is a new bit needed?
5. AETHER queue/audit extensions — new record type in `aether_audit.c`/`aether_queue.c` (Merkle chaining for S5, DAG `parent_action_id`)?
6. Scheduler/accounting hooks — `sched_exit`, `sched_create_state`, `agent_metrics_reap` (lineage departure summary, spawn-depth enforcement)?
7. Filesystem/root-mount constraints — depends on `root_mnt` (DDR-739), SFS dirs (DDR-738), or a new `fs_prefix`/namespace concept?
8. Network policy tables — touches `net_allow`, `CAP_NET`, `CAP_NET_BROWSE`, or the socket NSI (ADR-027)?
9. Compositor/UI exposure — new agent card/roster visual (Section G agents, Section F #73 sovereign UI)?
10. New smoke gate — what proves it **deterministically**? (avoid TCG-timing flakiness — DDR-735/771 lessons)
11. **Security invariant check (MANDATORY)** — which of S1–S8 govern this feature, and does the plan violate any of them even indirectly?

---

## Standing AETHER cognitive/ASI mandate

Long-term trajectory: subconscious background reasoning (F#71), verifier-based
self-improvement (F#72), the **metric lockbox** (F#68 / S3 — highest priority once
Section D is confirmed built, which it now is), and the full 12-agent Section G
roster. This is not a single task; it is an evolving capability goal tracked here.
Decompose into bounded DDRs only when a concrete slice is chosen.
