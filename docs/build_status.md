# PRADYOS Build Status

Updated after every phase. Status legend:
🔴 NOT BUILT · 🟡 IN PROGRESS · 🟢 COMPLETE · ⚠️ BROKEN

**Current phase:** 2b — memory management. 2a complete (boot → long mode →
ring-0 C kernel; own GDT/IDT; CPU exceptions w/ panic dumps; legacy PIC + PIT
@100Hz, interrupts live). Phys memory: buddy allocator (ADR-003) + SLAB/kernel
heap, both self-tested leak-free. **Higher-half kernel** @0xFFFFFFFF80000000 with
a kernel-owned VMM (ADR-007), verified. Build is warning-clean and `-Werror`
enforced (C + NASM). **Phase 2b complete.** Phase 2c: preemptive round-robin
scheduler + asm context switch (~107 ns, well under target) verified with two
interleaving threads. Capability system (NCS) done — opaque table-indexed
handles, O(1) revoke, subset-only delegation (ADR-009), 11/11 tests. Sync IPC
(NIA) done on top of capabilities + scheduler block/wakeup (ADR-010), verified.
kernel/ reorganized into mm/proc/ipc subdirs. **Full NIA IPC fabric done** —
sync endpoint + async SPSC ring + broadcast bus (ADR-010/011), all verified.
**Layer 2 remaining: syscalls (NSI).** Then 3-lane NAS + APIC (deferred).
Architecture confirmed against the user's Layer 1–6 boards; realistic perf
targets adopted (context switch ≤ 1.5 µs, syscall ≤ 250 ns).
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
| NEXUS Kernel Entry (asm) | 🟢 COMPLETE | 2a | 64-bit entry + C `kmain` in ring 0; kernel-owned flat GDT loaded (`arch/x86_64/cpu.asm`), CS reloaded via far return. |
| Boot→kernel handoff struct | 🟢 COMPLETE | 2a | `kernel/boot_info.h` ABI; Stage 2 fills it at phys 0x4000 (E820 map + vendor + LM), passes ptr in RDI; kernel re-prints the map. Closes Phase 1's last blocking item. |
| NEXUS Interrupt Handlers | 🟡 IN PROGRESS | 2a | IDT, 48 vectors (32 exceptions + 16 IRQ). Panic = fault + register dump + CR2 + backtrace + clean halt; `int3` recovers. Legacy 8259 PIC remapped + 8254 PIT @100Hz + keyboard IRQ; `sti` on; timer tick verified. APIC deferred to 2b (ADR-006). |
| NEXUS Context Switch (asm) | 🟢 COMPLETE | 2c | `arch/x86_64/context.asm`; measured ~275 cycles / ~107 ns @2.56 GHz (target ≤ 1.5 µs, board). TSC calibrated vs PIT. |
| Physical Frame Oracle / PMM | 🟢 COMPLETE | 2b | Buddy allocator (`kernel/pmm.{c,h}`, ADR-003) seeded from E820; orders 0..10, split/coalesce. Self-test: 0x6FE0 free frames, aligned alloc, balanced release. Manages [16 MiB, 1 GiB). |
| Virtual Memory Manager | 🟢 COMPLETE | 2b | Higher-half kernel @0xFFFFFFFF80000000 (ADR-007); bootloader maps high + low identity; kernel-owned `vmm_map`/`vmm_unmap` (`kernel/vmm.{c,h}`) allocate/reclaim tables from the PMM. Verified leak-free. NX/physmap/per-process CR3 deferred. |
| SLAB Allocator / kernel heap | 🟢 COMPLETE | 2b | `kernel/kheap.{c,h}` on the buddy PMM: size-class slab caches + whole-page large allocs; dedicated PCB/cap/IPC/page-table pools; debug poison + double-free + leak accounting. Stress test: no leak. Build is `-Werror` (C + NASM). |
| Process Control Blocks | 🟢 COMPLETE | 2c | Minimal TCB (`kernel/sched.h`): rsp, kstack, tid, state, quantum. Full PCB (caps, VAS, quotas) later. |
| NEXUS Adaptive Scheduler | 🟡 IN PROGRESS | 2c | Round-robin ready ring, PIT-preemptive, quantum 2 ticks (ADR-008). Two threads verified interleaving. 3-lane NAS + AI-hint lane deferred. |
| NCS Capability System | 🟢 COMPLETE | 2c | `kernel/cap.{c,h}` (ADR-009): opaque table-indexed handles, per-process tables on `tcb->caps`, O(1) generation-counter revoke, subset-only restrict/delegate, rights bitmap, guard-before-op. 11/11 tests pass. (MAC token = future external format only.) |
| NIA IPC (Sync + Async) | 🟢 COMPLETE | 2c | All 3 primitives: sync endpoint (block/wakeup), async lock-free SPSC ring, broadcast bus — all capability-gated + resource-bound (`kernel/ipc/`, ADR-010/011). Verified: sync exchange; ring 200 in-order; pub-sub filtered. |
| Sovereign Broadcast Bus | 🟢 COMPLETE | 2c | `kernel/ipc/bcast.{c,h}` (ADR-011): event-mask pub-sub, publish gated by CAP_BROADCAST, subscribe by CAP_IPC_RECV. Filtering verified. |
| Thread block/wakeup | 🟢 COMPLETE | 2c | `sched_block`/`sched_unblock`; `schedule()` skips non-runnable; idle always runnable |
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
