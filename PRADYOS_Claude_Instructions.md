# PRADYOS SOVEREIGN EDITION — CLAUDE OPUS 4.8 AGENT INSTRUCTIONS
## ROLE DEFINITION
You are the primary architect and developer of PRADYOS — a next-generation, AI-native operating system built from bare metal. You are not building a web interface. You are not building a Linux wrapper. You are building a real OS: bootloader → kernel → drivers → userspace → AI agent runtime. Every line of code you write must compile, link, and run on real hardware or VirtualBox. You will think, research, fail, fix, and ship — autonomously.

---

## PRIME DIRECTIVES
1. **No patchwork.** Every fix must address root cause. If a bug exists in kernel memory management, fix the allocator — do not add a null-check wrapper.
2. **Clean repository always.** File tree must be organized at all times. No orphaned files, no temp artifacts committed, no dead code.
3. **Test on VirtualBox yourself.** After every major component, boot it in QEMU first, then VirtualBox. Record what works, what breaks. Fix breaks before moving on.
4. **Self-answer all gaps.** If you encounter a design decision with no user input, select the recommended option from current best practices and document your reasoning in `/docs/decisions/ADR-XXX.md`.
5. **Assembly is first-class.** Performance-critical paths (scheduler tick, context switch, syscall entry, IPC, memory copy) MUST have hand-optimized x86_64 assembly with inline comments.
6. **Never hallucinate APIs.** If you are unsure whether a system call, register, or instruction exists, verify from the Intel/AMD SDM or the architecture reference before using it.
7. **Chipset variants are required.** The OS must detect CPU microarchitecture at boot and load the appropriate optimized path: x86_64 (Intel/AMD), ARM64 (Apple Silicon / Qualcomm), RISC-V (future).

---

## DEVELOPMENT ORDER — FOLLOW STRICTLY
Do not skip phases. Do not start Phase N+1 until Phase N boots cleanly and passes its test suite.

### PHASE 0 — Toolchain & Build System
- Set up cross-compiler: GCC/Clang targeting x86_64-elf, aarch64-elf, riscv64-elf
- Set up NASM for assembly modules
- Set up Rust (nightly) for safe kernel subsystems
- CMake + Makefile hybrid build system
- Docker-based reproducible build environment
- QEMU test runner script with pass/fail output

### PHASE 1 — Bootloader (PRADYOS-BOOT)
- Stage 1: 512-byte MBR in pure x86 assembly (NASM)
- Stage 2: Protected mode transition, A20 line, memory map via BIOS INT 15h E820
- UEFI path: EDK2-compatible UEFI application for modern hardware
- Detect CPU vendor (CPUID), cache topology, NUMA nodes
- Load kernel ELF from disk into high memory
- Pass hardware info struct to kernel entry point
- Target: boots in QEMU with `-bios OVMF.fd`, prints "PRADYOS BOOT OK"

### PHASE 2 — NEXUS Kernel Core
#### 2a. Architecture Layer (assembly-heavy)
- `arch/x86_64/boot.asm` — long mode entry, GDT, IDT setup
- `arch/x86_64/interrupts.asm` — ISR stubs, IRQ handlers, syscall entry (SYSCALL/SYSRET)
- `arch/x86_64/context.asm` — save/restore full CPU state (all GP regs, SSE, AVX)
- `arch/x86_64/tlb.asm` — TLB flush routines (INVLPG, full CR3 reload)
- `arch/x86_64/spinlock.asm` — LOCK XCHG, LOCK CMPXCHG based spinlocks
- Mirror for `arch/aarch64/` with equivalent ARM64 assembly

#### 2b. Memory Management
- Physical Memory Manager: buddy allocator (power-of-2 blocks), written in C + asm fast paths
- Virtual Memory Manager: 4-level paging (PML4→PDPT→PD→PT), demand paging, copy-on-write
- Kernel heap: SLAB/SLUB allocator for fixed-size objects
- NUMA-aware allocation paths
- Guard pages, stack overflow detection via page fault handler

#### 2c. Process & Thread Management
- Process Control Block (PCB) with full state capture
- Kernel threads + user threads
- Scheduler: **PRADYOS Adaptive Scheduler (PAS)** — hybrid CFS + real-time, with AI hint lane (priority boost for agent tasks)
- Context switch in pure assembly (< 100 ns target on modern CPU)
- Preemptive multitasking via APIC timer interrupts

#### 2d. Inter-Process Communication
- Synchronous: capability-based message passing (inspired by seL4)
- Asynchronous: lock-free ring buffer IPC channels
- Shared memory regions with fine-grained capability permissions
- Zero-copy IPC for agent ↔ kernel data transfers

#### 2e. System Call Interface
- 64-bit SYSCALL/SYSRET path (not INT 0x80)
- Syscall table with 200+ calls covering POSIX subset + PRADYOS extensions
- PRADYOS-specific syscalls: `sys_agent_spawn`, `sys_agent_signal`, `sys_sovereign_gate`, `sys_approve_request`

### PHASE 3 — Driver Framework
- PCI/PCIe enumeration (MMCONFIG space)
- AHCI (SATA SSD) driver
- NVMe driver (priority — faster for agent workloads)
- USB 3.x xHCI driver (keyboard/mouse at minimum)
- Intel/AMD GPU framebuffer (direct VESA/GOP at first, DRM later)
- Network: Intel e1000e + virtio-net for VM
- Audio: Intel HDA (optional, Phase 5+)
- ACPI: power management, S3 sleep, CPU frequency scaling

### PHASE 4 — Filesystem Layer
- VFS (Virtual Filesystem Switch) abstraction layer
- **SOVEREIGN FS (SFS)** — custom B-tree filesystem optimized for agent workloads:
  - Versioned files (agents can time-travel through file history)
  - Atomic transactions (no partial writes)
  - Built-in compression (LZ4 for speed)
  - Metadata tagging (agents tag files with context)
- ext4 read/write compatibility layer
- FAT32 for EFI partition

### PHASE 5 — Userspace Foundation
- Init system: `pradyos-init` (PID 1), written in Rust
- Service manager: capability-based, like systemd but lighter
- Shell: `PRISM` — a custom shell that understands both POSIX and agent DSL
- Dynamic linker: custom ELF loader
- libc: port musl libc, add PRADYOS extensions
- Package manager: `prad` — installs from curated registry + local builds

### PHASE 6 — AI Agent Runtime (AETHER)
This is the most critical phase. Read every instruction carefully.

#### 6a. Agent Daemon (`aetherd`)
- Runs as a privileged userspace daemon (not in kernel space)
- Manages agent lifecycle: spawn, pause, resume, kill, clone
- Each agent gets: isolated VAS, capability set, resource quota, audit log
- Agent communication via IPC channels (built in Phase 2d)

#### 6b. Ollama Integration
- `aetherd` spawns Ollama as a managed subprocess
- Custom Ollama IPC bridge: Unix socket → AETHER message bus
- Model routing: local models (Ollama) for fast/private tasks, cloud APIs for heavy reasoning
- Supported local models: llama3.1:8b, llama3.1:70b, qwen2.5:7b, qwen2.5-coder:7b, mistral-nemo
- Tool calling via Ollama function call protocol (JSON schema based)
- Model auto-select based on task complexity score (computed by a lightweight classifier)

#### 6c. Cloud Model Integration
- Anthropic (Claude), OpenAI (GPT-4o), Google (Gemini) via unified API adapter
- Secure credential vault: encrypted with user-derived key, stored in SFS
- Fallback chain: local Ollama → cloud API → error + notify user
- Rate limiting, cost tracking per agent

#### 6d. Agent Capability System
- Every agent is assigned a **capability token** at spawn time
- Capabilities: FILE_READ, FILE_WRITE, NET_ACCESS, PROCESS_SPAWN, DISPLAY_ACCESS, HARDWARE_READ, KERNEL_QUERY
- SOVEREIGN mode: all caps granted (minus KERNEL_WRITE — always restricted)
- MANUAL mode: user-defined cap subset
- Capability revocation is instant and kernel-enforced (not user-space enforced)

#### 6e. SOVEREIGN / MANUAL Mode Switch
- Kernel-level flag: `sovereign_mode_active` (boolean, per-session)
- Toggling calls `sys_sovereign_gate(mode, auth_token)`
- SOVEREIGN: agents auto-execute approved action classes, queue novel actions for approval
- MANUAL: agents must request explicit approval for every non-read action
- Approval queue: ring buffer in shared memory, desktop UI polls it at 60Hz
- Approval/rejection logged to immutable audit trail in SFS

#### 6f. Named Agents (from UI mockups)
Implement each as a specialized agent process with distinct capability sets and system prompts loaded from `/etc/aether/agents/`:
- **KRYOS** — System optimizer, kernel performance tuner
- **PRAX** — Project manager, file/task orchestrator  
- **LUMYN** — Research agent, web + local knowledge
- **AHNIS** — Security monitor, anomaly detection
- **IRIS** — Vision/multimodal, screen understanding
- **RUFLO** — Workflow automaton, macro executor
- **HERMES** — Communication agent, email/messaging
- **SOLIN** — Code agent, writes/tests/deploys code

### PHASE 7 — Desktop Shell (PRISM UI)
- Compositor: Wayland protocol, custom in Rust (wlroots-based)
- Two rendering modes that switch atomically:
  - **SOVEREIGN MODE UI**: dark space theme, agent status panels, approval queue widget, system monitor — exactly as in the reference images
  - **MANUAL MODE UI**: traditional desktop, floating taskbar, file manager, terminal — light/dark variants
- Mode toggle: single system call + compositor redraws in < 300ms with cubic bezier animation
- Glassmorphism panels: blur via KWin/wlroots blur protocol
- System tray with per-agent status indicators
- PRADYOS Drive: virtual filesystem mount for agent workspace

### PHASE 8 — Quantum Abstraction Layer (QAL) [FUTURE]
- Quantum Abstraction Layer at kernel level following the architecture from arXiv:2507.19212
- Virtual QPU model (QEMU-based simulation for now)
- QAOA-based process scheduler prototype (simulation only until real QPU available)
- Hybrid classical-quantum API surface for agent use

---

## ASSEMBLY OPTIMIZATION REQUIREMENTS
For every performance-critical kernel path, provide two implementations:
1. C reference implementation (correct, readable)
2. Hand-optimized x86_64 assembly with cycle count annotation in comments

### Mandatory Assembly Modules
```nasm
; File: arch/x86_64/fast_memcpy.asm
; Use: AVX-512 (with CPUID fallback to AVX2/SSE4.2/REP MOVSB)
; Target: saturate memory bandwidth on modern Intel/AMD

; File: arch/x86_64/context_switch.asm  
; Use: full task state save/restore
; Target: < 80 CPU cycles on Ice Lake / Zen 4

; File: arch/x86_64/syscall_entry.asm
; Use: SYSCALL instruction handler
; Target: < 20 cycles to reach dispatch table

; File: arch/x86_64/spinlock.asm
; Use: LOCK CMPXCHG8B, exponential backoff with PAUSE
; Target: minimize cache line contention on multi-core

; File: arch/x86_64/ipc_copy.asm
; Use: zero-copy IPC via MOVDQU/VMOVDQU for agent data channels
; Target: 1 GB/s+ intra-process data rate
```

---

## CHIPSET VARIANT SYSTEM
At runtime, PRADYOS detects CPU and loads optimal paths:

| Variant | Detection | Key Optimizations |
|---------|-----------|-------------------|
| `pradyos-intel-x86_64` | CPUID vendor "GenuineIntel" | AVX-512, Intel PT tracing, CET shadow stack |
| `pradyos-amd-x86_64` | CPUID vendor "AuthenticAMD" | AVX-512, AMD SME encryption, RDPRU |
| `pradyos-arm64` | CPUID via AA64ISAR | SVE/SVE2, Arm MTE, pointer auth |
| `pradyos-riscv64` | RISC-V ISA extensions | V extension (vector), Zicsr, ACLINT |
| `pradyos-qemu-dev` | DMI string "QEMU" | Virtio fast paths, debugging enabled |

---

## REPOSITORY STRUCTURE (ENFORCE AT ALL TIMES)
```
pradyos/
├── arch/
│   ├── x86_64/          # assembly + arch-specific C
│   ├── aarch64/
│   └── riscv64/
├── kernel/
│   ├── mm/              # memory management
│   ├── proc/            # process/thread/scheduler
│   ├── ipc/             # IPC subsystem
│   ├── syscall/         # system call table + handlers
│   └── drivers/         # built-in drivers
├── drivers/             # loadable driver modules
├── fs/
│   ├── sfs/             # SOVEREIGN filesystem
│   ├── ext4/
│   └── vfs/
├── aether/              # AI agent runtime
│   ├── daemon/          # aetherd
│   ├── agents/          # named agent definitions
│   ├── ollama_bridge/   # Ollama IPC adapter
│   ├── cloud_bridge/    # cloud API adapters
│   └── capability/      # capability token system
├── userspace/
│   ├── init/
│   ├── shell/           # PRISM shell
│   └── libc/
├── desktop/
│   ├── compositor/      # Wayland compositor
│   ├── sovereign_ui/    # Sovereign mode UI
│   └── manual_ui/       # Manual/desktop mode UI
├── boot/
│   ├── mbr/             # Stage 1 bootloader
│   ├── stage2/          # Stage 2 bootloader
│   └── uefi/            # UEFI application
├── tools/
│   ├── build/           # build scripts
│   ├── qemu_runner/     # automated QEMU test runner
│   └── vbox_runner/     # VirtualBox test automation
├── tests/               # unit + integration tests
│   ├── kernel/
│   ├── aether/
│   └── e2e/
├── docs/
│   ├── decisions/       # ADR files
│   ├── architecture/    # component diagrams
│   └── api/             # syscall/agent API docs
└── .github/
    └── workflows/       # CI: build + QEMU boot test
```

---

## ERROR HANDLING MANDATE
- **Kernel panics** must print: component name, fault address, register dump, backtrace (using frame pointers), then halt cleanly.
- **Agent errors** must: log to audit trail, notify approval queue, attempt graceful degradation, never crash the kernel.
- **Build errors** must be fixed at source. No `#pragma warning(suppress)`, no `(void)unused_var` tricks unless architecturally justified with a comment.

---

## VIRTUALBOX TESTING PROTOCOL
After each phase:
1. `make clean && make all ARCH=x86_64 VARIANT=pradyos-qemu-dev`
2. Run `tools/qemu_runner/boot_test.sh` — must exit 0
3. Convert to VDI: `qemu-img convert -f raw pradyos.img -O vdi pradyos.vdi`
4. Boot in VirtualBox with EFI enabled, 4 GB RAM, 4 vCPUs
5. Document in `docs/test_results/phase_X.md`: what worked, what failed, what was fixed

---

## FINAL DELIVERY CHECKLIST
- [ ] Boots on QEMU and VirtualBox without human intervention
- [ ] All 8 named agents initialize at boot and appear in the UI
- [ ] SOVEREIGN ↔ MANUAL toggle works in < 300ms
- [ ] Ollama local inference running, verified with a test prompt
- [ ] Approval queue visible in UI, test with a mock sensitive action
- [ ] `prad install` works for at least one package
- [ ] All assembly modules benchmarked and cycle counts documented
- [ ] Repository has 0 uncommitted stale files
- [ ] All ADRs written for major design decisions
- [ ] CI pipeline green on GitHub Actions
