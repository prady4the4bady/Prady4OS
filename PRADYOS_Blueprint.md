# PRADYOS SOVEREIGN EDITION — FULL VISION BLUEPRINT
### For Claude Opus 4.8 — Unhinged, Unfiltered, Complete

---

## THE CORE IDEA — WHAT THIS ACTUALLY IS

PRADYOS is not Linux with a pretty face. PRADYOS is not a web app pretending to be an OS. PRADYOS is a **ground-up, bare-metal, AI-native operating system** with its own original kernel — the **NEXUS Kernel** — written from scratch in a hybrid of hand-optimized x86_64 assembly, C, and Rust. It has no code lineage from Linux, BSD, or any existing kernel. It is a clean-room design. This is not a clone. It is a successor.

The philosophical premise: **every operating system ever built was designed for programs. PRADYOS is designed for intelligence.** The kernel is not just a resource manager. It is the substrate on which autonomous AI agents live, breathe, plan, and execute. The user is not the operator of the machine — the user is the **sovereign** who approves or rejects what the machine proposes to do on their behalf.

This document is the vision. Claude will make all core architectural decisions. Claude will write all code. Claude will test everything. Claude will fix everything. Claude will never stop until the OS boots, runs agents, and looks exactly like the reference images provided.

---

## THE NEXUS KERNEL — ORIGINAL DESIGN

### Why Original?
No GPL licensing complications. No ABI compatibility constraints from legacy decisions made in 1991. No architectural compromises inherited from UNIX. PRADYOS inherits concepts but zero code from any existing kernel. The NEXUS Kernel is a **capability-based hybrid kernel** — sitting between microkernel purity and monolithic performance.

### Kernel Architecture Philosophy — The Hybrid Position

The research is conclusive on this: pure microkernels (seL4, Mach) have superior isolation and formal verifiability, but suffer IPC overhead that can reach 5-10% performance cost in message-heavy workloads. Pure monolithic kernels (Linux, classical BSD) are fast but a single buggy driver crashes the entire system. For an AI-agent OS where agents spawn hundreds of sub-processes, neither extreme is acceptable.

**NEXUS is a Structured Hybrid Kernel:**
- **Trusted Kernel Core (ring 0):** scheduler, memory management, IPC primitives, capability enforcement, syscall dispatch. ~15,000 lines. Formally auditable.
- **Trusted Driver Layer (ring 0, isolated segments):** critical drivers (NVMe, GPU framebuffer, network) run in kernel space but in memory-isolated segments with guard pages. A crash kills the segment, not the system.
- **Untrusted Services (ring 3):** filesystem servers, audio, USB HID, all AI agent processes. These communicate with the kernel core via fast typed IPC channels.
- **Agent Runtime (ring 3, capability-sandboxed):** AETHER daemon and all named agents run here with fine-grained capability tokens.

This gives PRADYOS: monolithic-class performance for critical paths, microkernel-class fault isolation for drivers and agents, and a formally auditable trusted base.

### NEXUS Kernel — Original Subsystems

#### 1. NEXUS Memory Architecture (NMA)
Not based on any existing allocator design. Three-tier structure:

**Tier 1 — Physical Frame Oracle (PFO)**
A bitmap + red-black tree hybrid that tracks every physical page frame. Unlike Linux's buddy allocator which uses pure power-of-2 blocks, PFO uses **variable-weight allocation** — it can satisfy requests for any size up to 512 contiguous frames without fragmentation, using a predictive coalescing algorithm that merges free regions during idle CPU cycles rather than on-demand. This eliminates worst-case allocation latency spikes that hurt AI inference workloads.

**Tier 2 — Agent Virtual Address Space (AVAS)**
Each agent process gets a private 128 TB virtual address space (using 48-bit VA on x86_64). The top 32 TB is reserved for the **Agent Workspace Mirror** — a read-only mapping of system state that agents can query without syscalls (inspired by vDSO but far more extensive). Agents read CPU load, memory pressure, network stats, and peer agent status directly from this mirror at nanosecond speed.

**Tier 3 — Sovereign Memory Pool (SMP)**
A pre-allocated, locked memory pool (configurable 2–64 GB) reserved exclusively for agent inference buffers and Ollama model weights. This memory is never swapped. It is NUMA-local to the GPU. It uses huge pages (2MB/1GB) to minimize TLB pressure during LLM token generation. The kernel enforces this pool boundary — no userspace process outside AETHER can touch it.

#### 2. NEXUS Adaptive Scheduler (NAS)
Not CFS. Not a round-robin. A **three-lane scheduler:**

**Lane A — Deterministic Lane (real-time)**
Fixed priority, preemptive. For: audio, GPU vsync, keyboard input, agent approval queue polling. Guaranteed latency < 500 μs.

**Lane B — Throughput Lane (batch)**
AI inference jobs, file I/O, background compilation. Scheduled using a **weighted fair queue** with dynamic weight adjustment based on historical completion rates. An agent that consistently finishes tasks fast gets higher weight — it earns throughput.

**Lane C — Interactive Lane (default)**
Everything else. Uses an **urgency-decay algorithm**: tasks start with high urgency, decay over time, get boosted when the user interacts with associated UI. This keeps the desktop snappy even when agents are running heavy workloads in the background.

**The AI Hint Lane:** Any agent can submit a scheduling hint via `sys_agent_hint(task_id, priority_class, deadline_ns)`. The scheduler is not obligated to honor it, but uses it as soft input to the NAS decision algorithm. Agents that repeatedly give accurate hints get their hints weighted higher — a trust-building mechanism between the scheduler and the agent.

#### 3. NEXUS Capability System (NCS)
Every resource in PRADYOS is accessed through a **capability token** — a 128-bit cryptographically unforgeable handle. No ambient authority. No UID-based permissions. If you do not hold the capability, you cannot access the resource, period. The kernel enforces this at every syscall boundary.

Capability token structure:
```
[64-bit resource ID] [32-bit permission bitmap] [16-bit generation counter] [16-bit MAC truncation]
```

Capabilities are inherited through explicit delegation. When an agent spawns a sub-agent, it can pass a subset of its own capabilities — it cannot grant more than it holds (no confused deputy attacks). Capability revocation is O(1): increment the generation counter on the resource, all old tokens immediately invalid.

#### 4. NEXUS IPC Architecture (NIA)
Three IPC primitives:

**Synchronous Call Gates:** Like seL4 IPC but with zero-copy data passing via shared memory descriptors. Sender passes a capability to a memory region; receiver maps it directly. No kernel-mediated copy. 1 GB/s+ transfer rate between processes on the same NUMA node.

**Async Event Channels:** SPSC (single-producer single-consumer) lock-free ring buffers, one per agent pair. Cache-line aligned, padded to prevent false sharing. Used for high-frequency agent telemetry and status updates.

**Sovereign Broadcast:** A publish-subscribe bus built into the kernel. Any process can subscribe to system-wide events: `AGENT_PROPOSAL`, `RESOURCE_ALERT`, `APPROVAL_REQUESTED`, `MODE_CHANGE`. The desktop UI subscribes to `APPROVAL_REQUESTED` to show the approval popup. This is how the entire SOVEREIGN/MANUAL mode system works without polling.

#### 5. NEXUS Syscall Interface (NSI)
64-bit SYSCALL/SYSRET only. No legacy INT 0x80. Syscall entry in pure assembly, reaches dispatch table in < 20 cycles. Syscall numbers are stable (ABI guarantee). Extended syscalls for PRADYOS:

```c
// Agent lifecycle
sys_agent_spawn(agent_def_t *def, cap_set_t *caps) → agent_id_t
sys_agent_signal(agent_id_t id, signal_t sig) → int
sys_agent_query(agent_id_t id, query_t *q, result_t *r) → int

// Sovereign gate
sys_sovereign_gate(mode_t mode, auth_token_t *token) → int
sys_approve_request(request_id_t rid, bool approved) → int
sys_get_approval_queue(approval_entry_t *buf, size_t n) → int

// Capability management  
sys_cap_create(resource_id_t rid, perm_t perms) → cap_token_t
sys_cap_delegate(cap_token_t cap, agent_id_t target) → int
sys_cap_revoke(resource_id_t rid) → int

// AI memory pool
sys_aism_alloc(size_t bytes, numa_node_t node) → void*
sys_aism_free(void *ptr) → int
sys_aism_pin_model(model_id_t mid, void *weights, size_t sz) → int
```

---

## SOVEREIGN FILESYSTEM (SFS) — ORIGINAL DESIGN

SFS is a purpose-built filesystem for AI agent workloads. Not ext4. Not BTRFS. Original.

**Key innovations:**
- **Versioned by default.** Every write creates a new version. Agents can read file history (`open(path, O_VERSION, 5)` for 5 versions back). No separate git needed for agent workspace.
- **Atomic transactions.** Groups of file operations either all succeed or all roll back. Agents writing a 50-file project update either complete atomically or leave the filesystem unchanged.
- **Metadata tagging.** Files carry a 4KB metadata block: agent-written tags, confidence scores, timestamps, provenance. `ls --tags` shows it. Agents use it for semantic file search without a separate vector database.
- **Inline LZ4 compression.** Transparent, per-block, hardware-accelerated where available. Agent knowledge bases compress 3–5x. NVMe bandwidth freed up for inference I/O.
- **B+ tree structure.** O(log n) lookup. 4KB leaf nodes aligned to SSD erase blocks for write amplification minimization.

---

## AETHER AGENT RUNTIME — FULL ARCHITECTURE

### The Big Picture
AETHER is to AI agents what the Linux kernel is to processes. It provides scheduling, isolation, memory, communication, and access control — but for intelligent agents running LLMs, not for dumb POSIX processes.

```
┌─────────────────────────────────────────────────────────┐
│                    USER APPROVAL LAYER                   │
│         (Desktop UI polls approval queue at 60Hz)        │
├─────────────────────────────────────────────────────────┤
│                  NAMED AGENTS (ring 3)                   │
│  KRYOS │ PRAX │ LUMYN │ AHNIS │ IRIS │ RUFLO │ HERMES │ SOLIN │
├─────────────────────────────────────────────────────────┤
│               AETHER DAEMON (aetherd)                    │
│  Agent Scheduler │ Capability Manager │ Audit Logger     │
│  Task Planner    │ Approval Queue     │ Model Router      │
├──────────────┬──────────────────────────────────────────┤
│  OLLAMA IPC  │         CLOUD API ADAPTER                 │
│  BRIDGE      │  Anthropic │ OpenAI │ Gemini │ Mistral    │
├──────────────┴──────────────────────────────────────────┤
│              NEXUS KERNEL (ring 0)                       │
│  NAS Scheduler │ NMA Memory │ NCS Capabilities │ NIA IPC  │
└─────────────────────────────────────────────────────────┘
```

### Ollama Integration — Deep Architecture

Ollama runs as a managed child process of `aetherd`. The bridge is not HTTP polling. It is a **Unix domain socket IPC channel** with a custom binary protocol for minimal latency:

```
Agent Request → AETHER Message Bus → Ollama Bridge → Ollama Unix Socket
                                                           ↓
                                                   [NEXUS NMA]
                                                   [Sovereign Memory Pool]
                                                   [Model weights pinned in hugepages]
                                                           ↓
Agent ← AETHER Message Bus ← Ollama Bridge ← Token stream response
```

**Model routing algorithm:**
```
task_complexity = classifier.score(task_description)  // local 1B classifier
if task_complexity < 0.4:
    route → ollama:qwen2.5:7b          // fast, local, private
elif task_complexity < 0.7:
    route → ollama:llama3.1:8b         // capable, local
elif task_complexity < 0.85:
    route → ollama:llama3.1:70b        // powerful local (if hardware allows)
else:
    route → cloud:claude-opus-4        // maximum intelligence, cloud
    // fallback if offline: ollama:llama3.1:70b with warning
```

**Supported local models (via Ollama):**
- `llama3.1:8b` — general tasks, fast, 8GB VRAM
- `llama3.1:70b` — complex reasoning, 40GB VRAM (enterprise config)
- `qwen2.5:7b` — general + tool calling, best local agent model
- `qwen2.5-coder:7b` — SOLIN agent (code tasks)
- `mistral-nemo` — multilingual, HERMES agent (communication)
- `llava:13b` — IRIS agent (vision/multimodal tasks)

**Cloud API adapter (unified interface):**
```rust
trait CloudModelAdapter {
    async fn complete(&self, messages: Vec<Message>, tools: Vec<Tool>) 
        -> Result<ModelResponse, AdapterError>;
}

// Implementations:
struct AnthropicAdapter { api_key: SecureString, model: String }
struct OpenAIAdapter    { api_key: SecureString, model: String }
struct GeminiAdapter    { api_key: SecureString, model: String }
```

All keys stored in encrypted SFS vault. Decrypted in-memory only when an API call is in flight. Zeroed from memory immediately after.

### Agent Capability Matrix

| Agent | Core Capability Set | Model (default) | Sovereign Max Caps |
|-------|--------------------|-----------------|--------------------|
| KRYOS | KERNEL_QUERY, HARDWARE_READ, PROCESS_SPAWN | qwen2.5:7b | + KERNEL_TUNE |
| PRAX | FILE_READ, FILE_WRITE, PROCESS_SPAWN | llama3.1:8b | + NET_ACCESS |
| LUMYN | NET_ACCESS, FILE_READ, FILE_WRITE | llama3.1:8b | + PROCESS_SPAWN |
| AHNIS | KERNEL_QUERY, FILE_READ, NET_READ | llama3.1:8b | + PROCESS_INSPECT |
| IRIS | DISPLAY_ACCESS, FILE_READ, FILE_WRITE | llava:13b | + CAMERA_ACCESS |
| RUFLO | FILE_READ, FILE_WRITE, PROCESS_SPAWN, INPUT_INJECT | qwen2.5:7b | + NET_ACCESS |
| HERMES | NET_ACCESS, FILE_READ, FILE_WRITE | mistral-nemo | + CONTACT_ACCESS |
| SOLIN | FILE_READ, FILE_WRITE, PROCESS_SPAWN, NET_ACCESS | qwen2.5-coder:7b | + DEPLOY_ACCESS |

### SOVEREIGN / MANUAL Mode — Technical Implementation

**Mode state is kernel-level.** It lives in a kernel data structure, not in userspace. No userspace process can fake it.

```c
// kernel/sovereign/mode.c
typedef struct {
    bool active;                    // sovereign mode on/off
    uint64_t session_token;         // changes on each toggle, prevents replay
    uint32_t auto_approve_classes;  // bitmask of action classes auto-approved
    uint32_t pending_count;         // items in approval queue
} sovereign_state_t;

// Only sys_sovereign_gate() can modify this struct
// Only processes holding CAP_SOVEREIGN can call sys_sovereign_gate()
// CAP_SOVEREIGN is granted only to: aetherd, desktop compositor
```

**Approval queue mechanics:**
```
Agent wants to: DELETE /home/sovereign/Projects/Nebula/  (irreversible)
→ aetherd checks: action_class = FILE_DELETE_IRREVERSIBLE
→ auto_approve_classes has this bit? 
  → SOVEREIGN: no (irreversible deletes always require approval)
  → MANUAL: no
→ push to approval_queue: {agent=PRAX, action="delete Projects/Nebula", risk=HIGH, deadline=30s}
→ Sovereign Broadcast event: APPROVAL_REQUESTED
→ Desktop UI receives event, shows popup within 1 frame (16ms)
→ User clicks Approve/Reject
→ sys_approve_request(rid, approved) called
→ aetherd unblocks PRAX, executes or aborts
```

**Actions that are ALWAYS auto-approved in SOVEREIGN mode:**
- File reads anywhere in agent workspace
- Web searches, API calls to whitelisted domains
- Creating new files, new branches, running tests
- Spawning child processes within agent's capability set
- Reading system state (CPU, memory, network)

**Actions that ALWAYS require approval (both modes):**
- Deleting files outside agent workspace
- Sending emails or messages
- Making purchases or financial transactions
- Installing system-wide packages
- Modifying kernel configuration
- Accessing other agents' capability sets

---

## UI REFERENCE — WHAT TO BUILD

Reference images show two modes clearly. Build both exactly.

### SOVEREIGN MODE UI (reference: dark purple/space theme)
- Full-screen immersive layout, no window decorations
- Top-left: PRADYOS logo + scorpion mascot, Sovereign Edition label
- Top-center: greeting "Good evening, Sovereign." with time-aware message
- Center: PRADYOS ask bar (universal agent input)
- Below ask bar: quick-launch icons for AI Terminal, Files, System Monitor, Agent Center, Projects, Reports
- Right panel: System Overview (CPU/GPU/RAM/Disk donut charts), Network Activity sparkline, AI Agents grid (8 agents with status indicators)
- Bottom: floating glassmorphic taskbar with magnify-on-hover
- Color palette: deep space black, electric violet (#7B2FFF), soft purple glow, star particle background
- All panels: glassmorphism (backdrop-filter: blur(20px), rgba(255,255,255,0.05) background)

### MANUAL MODE UI (reference: traditional desktop, light and dark variants)
- Traditional floating window paradigm
- File Manager open by default (Quick Access, folder tree, recent files)
- PRISM Terminal visible (neofetch output showing PRADYOS info)
- Right panel: system controls (volume, brightness, night light, WiFi, Bluetooth)
- Performance meters (CPU 12%, GPU 18%, RAM 32%, Disk 68% — live data)
- Floating taskbar at bottom with spring animation
- Light variant: teal/azure accent, daylight/cloud wallpaper
- Dark variant: deep teal, dusk/city wallpaper
- Amber/orange variant: sunset theme for evening auto-switch

### Mode Toggle Mechanics
- Top-center pill toggle: SOVEREIGN MODE ☀ ↔ MANUAL MODE 🖥
- Toggle calls `sys_sovereign_gate()` → kernel sets mode flag → compositor receives SOVEREIGN_BROADCAST → full UI redraw in 300ms with cubic-bezier(0.4, 0, 0.2, 1) easing
- Toggle is accessible from anywhere — keyboard shortcut: `Super + M`

---

## COMPONENT BUILD STATUS TRACKER

Claude must maintain this file at `/docs/build_status.md` and update it after every phase:

| Component | Status | Phase | Notes |
|-----------|--------|-------|-------|
| PRADYOS-BOOT Stage 1 (MBR) | 🔴 NOT BUILT | 1 | 512-byte NASM |
| PRADYOS-BOOT Stage 2 | 🔴 NOT BUILT | 1 | Protected mode, memory map |
| UEFI Boot Path | 🔴 NOT BUILT | 1 | EDK2-compatible |
| NEXUS Kernel Entry (asm) | 🔴 NOT BUILT | 2a | long mode, GDT, IDT |
| NEXUS Interrupt Handlers | 🔴 NOT BUILT | 2a | ISR stubs, APIC |
| NEXUS Context Switch (asm) | 🔴 NOT BUILT | 2a | < 80 cycle target |
| Physical Frame Oracle | 🔴 NOT BUILT | 2b | custom allocator |
| Virtual Memory Manager | 🔴 NOT BUILT | 2b | 4-level paging |
| SLAB Allocator | 🔴 NOT BUILT | 2b | kernel heap |
| Process Control Blocks | 🔴 NOT BUILT | 2c | PCB + lifecycle |
| NEXUS Adaptive Scheduler | 🔴 NOT BUILT | 2c | 3-lane + AI hints |
| NCS Capability System | 🔴 NOT BUILT | 2d | 128-bit tokens |
| NIA IPC (Sync + Async) | 🔴 NOT BUILT | 2d | zero-copy |
| Sovereign Broadcast Bus | 🔴 NOT BUILT | 2d | pub-sub kernel |
| Syscall Table (200+ calls) | 🔴 NOT BUILT | 2e | SYSCALL/SYSRET |
| PRADYOS Extended Syscalls | 🔴 NOT BUILT | 2e | agent + sovereign |
| NVMe Driver | 🔴 NOT BUILT | 3 | priority storage |
| PCIe Enumeration | 🔴 NOT BUILT | 3 | MMCONFIG |
| GPU Framebuffer (GOP) | 🔴 NOT BUILT | 3 | UEFI GOP first |
| Network Driver (virtio-net) | 🔴 NOT BUILT | 3 | VM first |
| ACPI Power Management | 🔴 NOT BUILT | 3 | CPU freq scaling |
| VFS Layer | 🔴 NOT BUILT | 4 | abstraction |
| SOVEREIGN FS (SFS) | 🔴 NOT BUILT | 4 | B+ tree, versioned |
| ext4 Compatibility | 🔴 NOT BUILT | 4 | read/write |
| pradyos-init (PID 1) | 🔴 NOT BUILT | 5 | Rust |
| PRISM Shell | 🔴 NOT BUILT | 5 | POSIX + agent DSL |
| musl libc port | 🔴 NOT BUILT | 5 | + PRADYOS ext |
| prad package manager | 🔴 NOT BUILT | 5 | |
| AETHER Daemon | 🔴 NOT BUILT | 6 | core agent runtime |
| Ollama IPC Bridge | 🔴 NOT BUILT | 6 | Unix socket |
| Cloud API Adapters | 🔴 NOT BUILT | 6 | Anthropic/OpenAI/Gemini |
| Agent Capability Enforcer | 🔴 NOT BUILT | 6 | kernel-backed |
| SOVEREIGN Gate Logic | 🔴 NOT BUILT | 6 | mode switching |
| Approval Queue System | 🔴 NOT BUILT | 6 | ring buffer + UI |
| KRYOS Agent | 🔴 NOT BUILT | 6f | system optimizer |
| PRAX Agent | 🔴 NOT BUILT | 6f | project manager |
| LUMYN Agent | 🔴 NOT BUILT | 6f | research |
| AHNIS Agent | 🔴 NOT BUILT | 6f | security monitor |
| IRIS Agent | 🔴 NOT BUILT | 6f | vision/multimodal |
| RUFLO Agent | 🔴 NOT BUILT | 6f | workflow automaton |
| HERMES Agent | 🔴 NOT BUILT | 6f | communication |
| SOLIN Agent | 🔴 NOT BUILT | 6f | code agent |
| Wayland Compositor | 🔴 NOT BUILT | 7 | wlroots-based Rust |
| SOVEREIGN MODE UI | 🔴 NOT BUILT | 7 | dark space theme |
| MANUAL MODE UI | 🔴 NOT BUILT | 7 | traditional desktop |
| Mode Toggle Animation | 🔴 NOT BUILT | 7 | 300ms cubic-bezier |
| Glassmorphism Renderer | 🔴 NOT BUILT | 7 | blur + transparency |
| Quantum Abstraction Layer | 🔴 NOT BUILT | 8 | future |
| Intel x86_64 Variant | 🔴 NOT BUILT | all | AVX-512, CET |
| AMD x86_64 Variant | 🔴 NOT BUILT | all | SME, RDPRU |
| ARM64 Variant | 🔴 NOT BUILT | all | SVE2, MTE |
| RISC-V64 Variant | 🔴 NOT BUILT | all | V extension |
| AVX-512 memcpy (asm) | 🔴 NOT BUILT | 2 | bandwidth saturating |
| Syscall Entry (asm) | 🔴 NOT BUILT | 2e | < 20 cycles |
| IPC Zero-Copy (asm) | 🔴 NOT BUILT | 2d | VMOVDQU |
| CI Pipeline (QEMU boot) | 🔴 NOT BUILT | 0 | GitHub Actions |

Status legend: 🔴 NOT BUILT | 🟡 IN PROGRESS | 🟢 COMPLETE | ⚠️ BROKEN

---

## ASI-APPROACHING AGENT DESIGN PRINCIPLES

The 8 named agents are not chatbots. They are **persistent, stateful, goal-directed processes** that run continuously, accumulate knowledge, and self-improve within their domain. Here is how to make them approach ASI-level capability within their scope:

### 1. Persistent Memory Architecture
Each agent has three memory tiers:
- **Hot memory (context window):** current task + last 20 interactions, in RAM
- **Warm memory (SFS versioned files):** compressed interaction history, project context, learned patterns — agents READ this at task start
- **Cold memory (RAG index):** vector embeddings of all past agent outputs, searchable — agents QUERY this for long-term recall

### 2. Self-Directed Research Loop
LUMYN and SOLIN implement a research loop that runs in background:
```
while sovereign_mode:
    topic = identify_knowledge_gap(current_projects)
    results = web_search(topic)
    synthesis = model.synthesize(results)
    store_to_cold_memory(synthesis)
    sleep(adaptive_interval)
```

### 3. Agent-to-Agent Collaboration Protocol
Agents can task each other via AETHER Message Bus:
```
SOLIN → PRAX: "I need the project spec for Nebula before I write tests"
PRAX → LUMYN: "Research competitor analysis for Nebula's domain"
LUMYN → SOLIN: "Here is the analysis, stored at /aether/workspace/nebula/research/"
SOLIN: continues with full context
```

### 4. Failure Learning
Every agent failure (wrong output, crashed task, rejected action) is:
1. Logged with full context to SFS audit trail
2. Fed back as a negative example to the agent's system prompt for the session
3. After 10 failures in a category, escalates to KRYOS for root cause analysis
4. KRYOS writes a patch to the agent's base system prompt in `/etc/aether/agents/`

### 5. Computer Use Integration
IRIS + RUFLO together implement full computer use:
- IRIS sees the screen (screenshots analyzed by llava:13b or Claude Vision)
- RUFLO controls mouse + keyboard via kernel INPUT_INJECT capability
- Together: they can operate ANY graphical application without an API
- Use cases: filling forms in legacy apps, reading PDFs, operating CAD tools, automated testing

---

## QUANTUM COMPUTING INTEGRATION ROADMAP

This is Phase 8. Do not build this in early phases. Design the interface now so future code can plug in.

Based on arXiv:2507.19212, the architecture is:

**Quantum Abstraction Layer (QAL)** at kernel level:
- Virtual QPU device: `/dev/qpu0` — accessible via standard ioctl interface
- Quantum circuit submission: `ioctl(qpu_fd, QPU_SUBMIT_CIRCUIT, &circuit_desc)`
- Currently: backed by QEMU-simulated virtual QPU
- Future: backed by real QPU over PCIe or network

**Quantum-accelerated OS subsystems (future):**
- Process scheduler: QAOA optimization for multi-objective scheduling (latency + energy + throughput)
- Memory allocation: QAOA-based placement for NUMA-optimal allocation
- File search: Grover's algorithm via QFSI for O(√N) file lookup
- Network routing: QAOA for shortest-path agent communication

**Why this matters for PRADYOS agents:**
When agents face NP-hard optimization problems (optimal task ordering, resource allocation across 8 agents, travel planning), the QAL gives them access to quantum speedup — not for general intelligence, but for specific combinatorial optimization tasks where quantum provides provable advantage.

---

## PERFORMANCE TARGETS

These are not aspirational. Claude must benchmark and document actual measurements.

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Boot to agent login | < 3 seconds | `ktime` from power-on |
| Context switch latency | < 100 ns | rdtsc before/after |
| Syscall round-trip | < 200 ns | null syscall benchmark |
| IPC throughput (same NUMA) | > 10 GB/s | memcpy benchmark via IPC |
| Agent spawn time | < 50 ms | time from `sys_agent_spawn` to first token |
| Mode toggle animation | < 300 ms | frame timestamp in compositor |
| Ollama first token (7B) | < 500 ms | aetherd request timestamp |
| File open (SFS, warm cache) | < 1 μs | fio benchmark |
| Approval popup latency | < 16 ms | event timestamp to frame rendered |

---

## DESIGN CONSTRAINTS — WHAT TO ABSOLUTELY AVOID

1. **No GPL licensed kernel code.** Not a single line. Write everything original or use MIT/Apache2/BSD-2 licensed components.
2. **No Node.js, Electron, or web technologies in the kernel or agent runtime.** Web UI only in the desktop shell, isolated from system components.
3. **No polling loops for critical paths.** Use interrupts, event channels, and async I/O everywhere.
4. **No memory leaks. Ever.** Use Rust for all heap-managed userspace components. Use explicit ownership in C kernel code. Static analysis tools must be part of the build pipeline.
5. **No hardcoded paths.** Everything configurable through capability-controlled config files.
6. **No single points of failure.** If SOLIN crashes, the kernel does not crash. If Ollama crashes, aetherd restarts it. If aetherd crashes, init restarts it. Only a kernel panic is unrecoverable.

---

## THE VISION IN ONE PARAGRAPH

PRADYOS is the operating system that treats human intelligence and machine intelligence as equal partners on the same substrate. The user sits at the top as the Sovereign — setting direction, approving the consequential, and observing the extraordinary. Below the user: eight specialized AI agents that work in parallel, communicate with each other, accumulate knowledge over time, and execute at superhuman speed across every domain of digital work. Below the agents: the NEXUS Kernel — an original, capability-secured, assembly-optimized, AI-native kernel that gives agents the memory, scheduling priority, and IPC speed they need to perform at their ceiling. The result is not a computer that waits for instructions. It is a machine that thinks alongside you, proposes before you ask, executes while you approve, and never stops working on what matters.

Build it.
