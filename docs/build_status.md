# PRADYOS Build Status

Updated after every phase. Status legend:
🔴 NOT BUILT · 🟡 IN PROGRESS · 🟢 COMPLETE · ⚠️ BROKEN

**Current phase:** 2a — NEXUS kernel entry. Full chain boots: Stage 1 → Stage 2
(A20, E820, CPUID) → 32-bit PM (`PRADYOS BOOT OK`) → 4-level paging + long mode →
64-bit ring-0 C kernel printing `NEXUS KERNEL OK` (smoke PASS on the kernel
sentinel). Next 2a slices: hardware-info handoff struct, GDT/IDT in kernel +
exception handlers, then context switch. See ADR-005.
**Last updated:** 2026-06-17

## Phase 0 — Toolchain & Build System

Verified on WSL2 Ubuntu-22.04, 2026-06-17: clang 14.0.0, LLD 14.0.0,
NASM 2.15.05, QEMU 6.2.0, rustc/cargo 1.98.0-nightly (target
`x86_64-unknown-none`). `make toolchain-check` and `make smoke` both exit 0.

| Item | Status | Notes |
|------|--------|-------|
| Repo skeleton (mandated tree) | 🟢 COMPLETE | dirs + `.gitkeep`, ADRs, docs |
| Toolchain bootstrap script | 🟢 COMPLETE | `tools/build/setup_toolchain.sh` (WSL2), run OK |
| LLVM/Clang + lld + NASM + Rust nightly | 🟢 COMPLETE | installed + `make toolchain-check` links clean (exit 0) |
| x86_64 triple pinned | 🟢 COMPLETE | `x86_64-elf` confirmed on clang 14 (see ADR-001) |
| QEMU runner harness | 🟢 COMPLETE | `tools/qemu_runner/boot_test.sh` SKIPs cleanly (inert until P1) |
| CI Pipeline (QEMU boot) | 🟡 IN PROGRESS | `.github/workflows/ci.yml` written; local equiv passes, but CI unrun until first push to GitHub |

## Component Tracker (Phases 1–8)

| Component | Status | Phase | Notes |
|-----------|--------|-------|-------|
| PRADYOS-BOOT Stage 1 (MBR) | 🟢 COMPLETE | 1 | 512-byte MBR; INT 13h/AH=42h LBA read loads Stage 2, jumps to it. Kernel-ELF load deferred to Phase 2a (no kernel yet). |
| PRADYOS-BOOT Stage 2 | 🟢 COMPLETE | 1 | A20 (fast), INT 15h E820 walk, CPUID vendor + long-mode bit, flat GDT, 32-bit protected-mode switch, `PRADYOS BOOT OK` via COM1 + VGA. `make smoke` PASS. |
| UEFI Boot Path | 🔴 NOT BUILT | 1 | EDK2/OVMF-compatible; deferred (MBR path chosen first per user) |
| Hardware-info handoff struct | 🔴 NOT BUILT | 1 | E820 + CPUID gathered but not yet packaged for a kernel; blocked on Phase 2a |
| NEXUS Kernel Entry (asm) | 🟡 IN PROGRESS | 2a | 64-bit entry (`arch/x86_64/boot.asm`) + C `kmain` run in ring 0; long mode reached via bootloader paging. Kernel-owned GDT + IDT still TODO. |
| Boot→kernel handoff struct | 🔴 NOT BUILT | 2a | E820/CPUID gathered in Stage 2 but not yet passed to the kernel (closes Phase 1 item) |
| NEXUS Interrupt Handlers | 🔴 NOT BUILT | 2a | IDT, CPU exception stubs first, then APIC |
| NEXUS Context Switch (asm) | 🔴 NOT BUILT | 2a | target TBD (see ADR) |
| Physical Frame Oracle / PMM | 🔴 NOT BUILT | 2b | allocator design OPEN — see ADR-003 |
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
| Ollama IPC Bridge | 🔴 NOT BUILT | 6 | feasibility OPEN — see ADR-004 |
| Cloud API Adapters | 🔴 NOT BUILT | 6 | Anthropic/OpenAI/Gemini |
| Agent Capability Enforcer | 🔴 NOT BUILT | 6 | kernel-backed |
| SOVEREIGN Gate Logic | 🔴 NOT BUILT | 6 | mode switching |
| Approval Queue System | 🔴 NOT BUILT | 6 | ring buffer + UI |
| Named Agents (KRYOS…SOLIN) | 🔴 NOT BUILT | 6f | 8 agents |
| Wayland Compositor | 🔴 NOT BUILT | 7 | wlroots-based Rust |
| SOVEREIGN MODE UI | 🔴 NOT BUILT | 7 | dark space theme |
| MANUAL MODE UI | 🔴 NOT BUILT | 7 | traditional desktop |
| Mode Toggle Animation | 🔴 NOT BUILT | 7 | 300ms cubic-bezier |
| Quantum Abstraction Layer | 🔴 NOT BUILT | 8 | future; citation unverified |
| AVX-512 memcpy (asm) | 🔴 NOT BUILT | 2 | with CPUID fallback |
| Syscall Entry (asm) | 🔴 NOT BUILT | 2e | target TBD (see ADR) |
| IPC Zero-Copy (asm) | 🔴 NOT BUILT | 2d | VMOVDQU |
