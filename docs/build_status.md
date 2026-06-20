# PRADYOS Build Status

Updated after every phase. Status legend:
🔴 NOT BUILT · 🟡 IN PROGRESS · 🟢 COMPLETE · ⚠️ BROKEN

Target ISAs, platform profiles (x86_64 / ARM64 incl. Grace Blackwell / Apple /
RISC-V64), the 8-layer map, and the branch strategy live in
[platform_profiles.md](platform_profiles.md).

**Current phase:** 5 — userspace. Layer 2 (NEXUS kernel core) COMPLETE: boot →
long mode → ring-0 C kernel; own GDT/IDT; CPU exceptions w/ panic dumps; legacy
PIC + PIT @100Hz; buddy PMM (ADR-003) + SLAB heap (leak-free); higher-half
kernel @0xFFFFFFFF80000000 + kernel-owned VMM (ADR-007); preemptive round-robin
scheduler + asm context switch (~107 ns); NCS capability system (ADR-009, 11/11
tests); full NIA IPC fabric (sync + async ring + broadcast, ADR-010/011);
syscalls (NSI) + ring-3 user mode (ADR-012, SYSCALL/SYSRET, TSS, cap-gated).
**Phase 3 (Layer 3 drivers) COMPLETE:** ACPI parser + PCIe MMCONFIG enumeration
(ADR-013, 7 devices on q35); reusable modern-1.0 virtio transport + generic
block layer + interrupt-driven virtio-blk (ADR-014); CMOS RTC (ADR-020).
**Phase 4 (Layer 4 FS) COMPLETE:** capability-gated VFS with a mount table +
per-mount context (ADR-017, FAT32/SFS/ext4 side-by-side). **FAT32 read-write**
(ADR-015): nested paths (4a/4b), create/write/unlink with all-or-nothing alloc +
read-back verify (4c), VFAT long-name read + RTC timestamps (4j, ADR-020).
**SOVEREIGN FS (SFS) engine** (ADR-018): in-kernel format/mount (4d), CoW B+tree
create/lookup/open/readdir (4e), file extents (4f), journal + atomic transactions
with crash replay (4g), snapshots/version isolation (4h), per-extent inline LZ4 +
metadata tags (4i). **ext4 read-only** (ADR-019, 4j). Shared kernel state
(PMM/console/scheduler) made preemption-safe (ADR-016). **Layer 4 completion gate
PASSED** — all 5 gates green (smoke, smoke-fs, smoke-fs-rw, smoke-fs-sfs-rw,
smoke-fs-ext4) on CI. **Phase 5 (Layer 5 userspace) IN PROGRESS — slice 5a
(static ELF loader + W^X) COMPLETE** (ADR-021): per-process address spaces
(`vmm_new_address_space`/`vmm_map_in`/`vmm_destroy_address_space`, kernel
higher-half shared, user range = PML4 slot 1); per-process CR3 switched on
context switch; **EFER.NXE enabled (CPUID-gated)** so VMM_NX/W^X is honored;
ELF64 loader (`kernel/exec/elf.c`) maps each PT_LOAD with permissions derived
from p_flags (text RX, rodata R-NX, data RW-NX; W+X rejected), 8 MiB RW-NX stack
+ unmapped guard page, SysV argv/envp/auxv frame; a ring-3 fault is turned into a
clean process kill (no kernel panic). The test program is written to SFS and
**loaded back from SFS**; it prints `HELLO FROM RING-3` and exits via sys_exit. A
W^X negative regression (write to RX text → #PF err=0x7 → clean kill, kernel
survives) is asserted. New gate `smoke-user` PASS; all 5 prior FS gates still
green. Next: 5b (~50 POSIX syscalls). Build is warning-clean and `-Werror`
enforced (C + NASM). Deferred: 3-lane NAS, APIC, kernel-self W^X, COW fork,
process reaping/AS-teardown on exit, dynamic linking, SFS free-space-tree/GC,
ext4 write (see DEFERRED). Architecture confirmed against the user's Layer 1–6
boards; realistic perf targets adopted (context switch ≤ 1.5 µs, syscall ≤ 250 ns).
**5b design recorded (ADR-022 = DDR-5b):** validated user-pointer copy contract
(`copyin`/`copyout`/`copyinstr` → EFAULT, never CPL-0 panic), NSI-v2 table/ABI
(6-arg, own number space, fd↔capability bridge), 9 syscall clusters in build
order, fork = copy-all-pages (COW deferred to a future ADR), mmap MAP_ANON
baseline. **Slice 5b-2 (uaccess primitives) COMPLETE:** `kernel/mm/uaccess.c`
(`copyin`/`copyout`/`copyinstr`) on `vmm_user_range_ok` — walks the calling
process's page tables WITHOUT dereferencing the user address, so a wild pointer
or a read-only-page write returns `-EFAULT` and the kernel never #PFs at CPL 0
(W^X upheld on the copy path). New gate `smoke-uaccess` PASS (good page, wild
ptr → EFAULT, RO-page write → EFAULT, valid string). **Slice 5b-3
(sys_read/sys_write + per-process fd table) COMPLETE:** per-process `fd_table`
(`kernel/proc/fd.c`, FD_MAX=64) embedded in the TCB, fds 0/1/2 pre-wired to the
console for user threads; `sys_write` (`kernel/syscall/sys_io.c`) copies the user
buffer via `copyin` and writes to the console (-EBADF on bad fd, -EFAULT on bad
buffer, kernel survives); `sys_read` stubbed -ENOSYS until slice 4; NSI-v2 table
grown to 64, unknown call → -ENOSYS; `SYS_READ=5`/`SYS_WRITE=6`. New ring-3
`user/systest.asm` (grows per slice) drives these; new gate `smoke-sysio` PASS
(SYSWRITE OK / EBADF / EFAULT). Next: 5b-4 (sys_open/sys_close/sys_fstat).
**Last updated:** 2026-06-21

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
| CI Pipeline (QEMU boot) | 🟢 LIVE | `.github/workflows/ci.yml` runs on push to `main`/`dev/**` + PRs: toolchain-check, `make image` (-Werror), `make smoke` (q35 kernel-sentinel gate), `make smoke-fs` (q35 FAT32 read end-to-end; installs dosfstools+mtools). `smoke` deliberately does not depend on the FAT image so the kernel gate never fails on a missing host FS tool. |

## Component Tracker (Phases 1–8)

| Component | Status | Phase | Notes |
|-----------|--------|-------|-------|
| PRADYOS-BOOT Stage 1 (MBR) | 🟢 COMPLETE | 1 | 512-byte MBR; INT 13h/AH=42h LBA read loads Stage 2, jumps to it. Kernel-ELF load deferred to Phase 2a (no kernel yet). |
| PRADYOS-BOOT Stage 2 | 🟢 COMPLETE | 1 | A20 (fast), INT 15h E820 walk, CPUID vendor + long-mode bit, flat GDT, 32-bit protected-mode switch, `PRADYOS BOOT OK` via COM1 + VGA. `make smoke` PASS. |
| UEFI Boot Path | 🔴 NOT BUILT | 1 | EDK2/OVMF-compatible; deferred (MBR path chosen first per user) |
| Hardware-info handoff struct | 🔴 NOT BUILT | 1 | E820 + CPUID gathered but not yet packaged for a kernel; blocked on Phase 2a |
| NEXUS Kernel Entry (asm) | 🟢 COMPLETE | 2a | 64-bit entry + C `kmain` in ring 0; kernel-owned flat GDT loaded (`arch/x86_64/cpu.asm`), CS reloaded via far return. |
| Boot→kernel handoff struct | 🟢 COMPLETE | 2a | `kernel/boot_info.h` ABI; Stage 2 fills it at phys 0x4000 (E820 map + vendor + LM), passes ptr in RDI; kernel re-prints the map. Closes Phase 1's last blocking item. |
| NEXUS Interrupt Handlers | 🟡 IN PROGRESS | 2a/5a | IDT, 48 vectors (32 exceptions + 16 IRQ). Panic = fault + register dump + CR2 + backtrace + clean halt; `int3` recovers. **5a:** a fault from CPL 3 (ring-3 user code) is converted to a clean process kill (`sched_exit`) with a `[trap]` diagnostic instead of a kernel panic — the W^X / guard-page enforcement path (ADR-021). Legacy 8259 PIC remapped + 8254 PIT @100Hz + keyboard IRQ; `sti` on; timer tick verified. APIC deferred to 2b (ADR-006). |
| NEXUS Context Switch (asm) | 🟢 COMPLETE | 2c | `arch/x86_64/context.asm`; measured ~275 cycles / ~107 ns @2.56 GHz (target ≤ 1.5 µs, board). TSC calibrated vs PIT. |
| Physical Frame Oracle / PMM | 🟢 COMPLETE | 2b | Buddy allocator (`kernel/pmm.{c,h}`, ADR-003) seeded from E820; orders 0..10, split/coalesce. Self-test: 0x6FE0 free frames, aligned alloc, balanced release. Manages [16 MiB, 1 GiB). |
| Virtual Memory Manager | 🟢 COMPLETE | 2b/5a | Higher-half kernel @0xFFFFFFFF80000000 (ADR-007); bootloader maps high + low identity; kernel-owned `vmm_map`/`vmm_unmap` (`kernel/mm/vmm.{c,h}`) allocate/reclaim tables from the PMM. Verified leak-free. **5a (ADR-021):** per-process address spaces (`vmm_new_address_space` shares kernel PML4 entries, user range = slot 1; `vmm_map_in` targets a non-active AS; `vmm_destroy_address_space` frees the private subtree); **EFER.NXE enabled, CPUID-gated** so `VMM_NX` is honored. physmap deferred. |
| SLAB Allocator / kernel heap | 🟢 COMPLETE | 2b | `kernel/kheap.{c,h}` on the buddy PMM: size-class slab caches + whole-page large allocs; dedicated PCB/cap/IPC/page-table pools; debug poison + double-free + leak accounting. Stress test: no leak. Build is `-Werror` (C + NASM). |
| Process Control Blocks | 🟢 COMPLETE | 2c | Minimal TCB (`kernel/sched.h`): rsp, kstack, tid, state, quantum. Full PCB (caps, VAS, quotas) later. |
| NEXUS Adaptive Scheduler | 🟡 IN PROGRESS | 2c/5a | Round-robin ready ring, PIT-preemptive, quantum 2 ticks (ADR-008). Two threads verified interleaving. **5a:** per-process CR3 in the TCB; `schedule()` reloads CR3 when switching address spaces (kernel stacks identity-mapped in every AS, so the switch precedes the stack switch safely). 3-lane NAS + AI-hint lane deferred. |
| NCS Capability System | 🟢 COMPLETE | 2c | `kernel/cap.{c,h}` (ADR-009): opaque table-indexed handles, per-process tables on `tcb->caps`, O(1) generation-counter revoke, subset-only restrict/delegate, rights bitmap, guard-before-op. 11/11 tests pass. (MAC token = future external format only.) |
| NIA IPC (Sync + Async) | 🟢 COMPLETE | 2c | All 3 primitives: sync endpoint (block/wakeup), async lock-free SPSC ring, broadcast bus — all capability-gated + resource-bound (`kernel/ipc/`, ADR-010/011). Verified: sync exchange; ring 200 in-order; pub-sub filtered. |
| Sovereign Broadcast Bus | 🟢 COMPLETE | 2c | `kernel/ipc/bcast.{c,h}` (ADR-011): event-mask pub-sub, publish gated by CAP_BROADCAST, subscribe by CAP_IPC_RECV. Filtering verified. |
| Thread block/wakeup | 🟢 COMPLETE | 2c | `sched_block`/`sched_unblock`; `schedule()` skips non-runnable; idle always runnable |
| Syscall Table (200+ calls) | 🟡 IN PROGRESS | 2e | SYSCALL/SYSRET armed (EFER.SCE/STAR/LSTAR/SFMASK), dispatch table + register, capability-aware (`kernel/syscall/`, ADR-012). 4 calls (putc/getpid/yield/exit) exercised from ring 3; more arrive with userspace. |
| Ring-3 user mode (TSS + user segs) | 🟢 COMPLETE | 2e | `kernel/proc/tss.c`, user GDT segs, `sched_create_user`, IRETQ→ring3, cap-gated syscalls. Verified user thread runs + exits cleanly. |
| PRADYOS Extended Syscalls | 🔴 NOT BUILT | 6 | agent + sovereign (Phase 6) |
| ACPI table parser (RSDP/RSDT/XSDT) | 🟢 COMPLETE | 3 | `kernel/acpi/` (ADR-013): find RSDP, walk RSDT/XSDT, `acpi_find_table`. Unblocks MCFG/MADT/FADT. |
| PCIe Enumeration | 🟢 COMPLETE | 3 | `kernel/drivers/pcie/` (ADR-013): MCFG→ECAM, uncached map, bus-0 scan + device registry. q35: 7 devices incl. virtio-blk/net + VGA. |
| virtio transport (reusable) | 🟢 COMPLETE | 3 | `kernel/drivers/virtio/` (ADR-014): modern 1.0 — PCI caps, BAR/MMIO, status machine, feature negotiation, split virtqueues, notify, ISR. Shared by all virtio devices. |
| Block layer (generic) | 🟢 COMPLETE | 3 | `kernel/drivers/blk/blk.{c,h}`: device registry + read/write dispatch. |
| virtio-blk driver | 🟢 COMPLETE | 3 | `kernel/drivers/blk/virtio_blk.c` (ADR-014/015): interrupt-driven (INTx) read/write. **Multi-instance** (per-disk transport/queue/BAR window) + **per-device serialization** (busy + yield-wait). Verified: sector-0 MBR read, write/read round-trip, 2 disks concurrently. |
| NVMe Driver | 🔴 NOT BUILT | 3 | priority storage (registers with blk layer) |
| GPU Framebuffer | 🔴 NOT BUILT | 3 | linear framebuffer |
| Network Driver (virtio-net) | 🔴 NOT BUILT | 3 | device enumerated; reuses virtio transport |
| ACPI Power Management (FADT/MADT) | 🔴 NOT BUILT | 3 | parser ready; MADT→APIC, FADT→power |
| VFS Layer | 🟢 COMPLETE | 4 | `kernel/fs/vfs/` (ADR-015): driver registry + **mount table** (per-mount context vtable; FAT32/SFS/ext4 mountable side-by-side) + `open`/`create`/`read`/`write`/`unlink`/`readdir`, all capability-gated (CAP_FS_READ/WRITE via NCS) + per-thread write budget. Full mount-point namespace deferred. |
| FAT32 (read-write) | 🟢 COMPLETE | 4 | `kernel/fs/fat32/` (ADR-015): BPB parse, FAT chain, 8.3 + **VFAT long-name read** (ADR-020), nested paths. Read-write (4c): create/write/unlink, all-or-nothing alloc, read-back verify (`smoke-fs-rw`). **Timestamps** from RTC (4j). LFN *write* deferred (creates 8.3). |
| RTC / CMOS clock | 🟢 COMPLETE | 3 | `kernel/drivers/rtc/` (ADR-020): wall-clock via ports 0x70/0x71 (BCD/binary, 12/24h, stable read). `rtc_now` + `rtc_fat_datetime`; powers FS timestamps and later CLOCK_REALTIME. (Deferred Layer-3 item, pulled in at 4j.) |
| SOVEREIGN FS (SFS) | 🟢 COMPLETE | 4 | `kernel/fs/sfs/` (ADR-017/018): inode-based CoW B+tree, 4 KiB blocks. **4d:** format/mount/empty-root. **4e:** CoW B+tree create/lookup/open/readdir (split-on-insert; 10-file test). **4f:** file extents (write append/grow + read). **4g:** journal + atomic transactions (commit-record + mount replay). **4h:** snapshots — retained CoW roots; `sfs_open_version` reads a file as-of a snapshot. **4i:** inline LZ4 (`kernel/fs/sfs/lz4.c`, bounds-checked) — **per-extent** compression so compressed files still append; + ~4 KiB inode metadata tags (`sfs_set_tag`/`get_tag`). Verified: 128 KiB compressible → <32 blocks, byte-exact readback, tag survives remount. Next: ext4 read + FAT32 LFN (4j) → Layer 4 gate. Free-space B+tree / snapshot GC deferred. `CAP_FS_SFS_*` reserved. |
| SOVEREIGN FS (SFS) | 🔴 NOT BUILT | 4 | B+ tree, versioned |
| ext4 Compatibility | 🟢 COMPLETE | 4 | `kernel/fs/ext4/` (ADR-019, slice 4j): **read-only** (the Layer-4 scope; write is out of scope) — superblock, group descriptors, extent-mapped inodes (depth-0), linear dir scan, nested paths. Verified reading a host `mkfs.ext4 -d` volume (4th disk). Write, multi-level extents, block-mapped inodes deferred. |
| ELF64 loader + W^X (static) | 🟢 COMPLETE | 5a | `kernel/exec/elf.c` (ADR-021): validates ET_EXEC/x86-64, maps each PT_LOAD into a fresh per-process AS with p_flags→W^X perms (text RX, rodata R-NX, data RW-NX; **W+X rejected**), zero-fills BSS, 8 MiB RW-NX user stack + unmapped guard page, SysV `argc/argv/envp/auxv` frame; spawns a ring-3 thread (cap delivered in RDI). Bootstrapped via SFS: the embedded test ELF (`user/hello.asm`) is written to SFS then **loaded back from SFS** — prints `HELLO FROM RING-3`, exits via sys_exit. W^X negative regression (`user/wxviol.asm`: write to RX text → #PF err=0x7 → clean kill, kernel survives). Gate `smoke-user` PASS. COW fork / dynamic linking / AS-reaping deferred. |
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
| Syscall Entry (asm) | 🟢 COMPLETE | 2e | `arch/x86_64/syscall_entry.asm` — stack switch, save/restore, marshal, SYSRET. |
| IPC Zero-Copy (asm) | 🔴 NOT BUILT | 2d | VMOVDQU |

## ⏸ DEFERRED — tracked, not dropped (interim implementation in place or later phase)

These are intentionally postponed. The current build is functionally complete
without them; each has a concrete "build before" trigger so it is not forgotten.

| Deferred item | Interim / status | Build before |
|---------------|------------------|--------------|
| **APIC** (Local + I/O APIC, APIC timer) | legacy 8259 PIC + PIT active | SMP; MSI device interrupts. Unblocked by the ACPI/MADT parser (Phase 3). |
| **3-lane NAS scheduler** (Determ./Throughput/Interactive + AI-hint) | round-robin placeholder (ADR-008) | differentiated agent workloads (Layer 6) |
| **Physical Frame Oracle** (variable-weight + predictive coalesce) | buddy PMM interim (ADR-003) | Sovereign Memory Pool / hugepages (Layer 6) |
| **Agent Virtual Address Space (AVAS)** (128 TB + RO state mirror) | — | AETHER bringup (Layer 6) |
| **Sovereign Memory Pool** (NUMA-pinned hugepages for weights) | — | AETHER/Ollama (Layer 6) |
| **User W^X / NX + guard pages** | ✅ DONE (5a, ADR-021): EFER.NXE on, per-segment perms, guard pages, ring-3 fault → clean kill | — |
| **Kernel-self W^X** (kernel text RX, kernel data NX) | bootloader maps the kernel image RWX; user W^X is fully enforced | kernel-hardening pass (remap kernel sections after boot) |
| **COW fork** | none yet (fork lands in 5b as copy-all-pages; COW after) | memory-heavy fork workloads |
| **Process reaping / AS teardown on exit** | `vmm_destroy_address_space` exists + used on load-failure paths; an exited user process leaks its AS + kernel stack until a reaper | PID-1 orphan reaper (5d) |
| **Dynamic linking** (`ld-pradyos.so`) | static ELF only | shared libraries |
| **UEFI / OVMF boot path** | legacy BIOS/MBR path complete | ARM64 / RISC-V variants (Layer 1) |
| **AVX-512 asm + IPC zero-copy (VMOVDQU)** | scalar paths | performance-hardening pass |
| **Larger/relocating kernel loader** | Stage 2 loads 256 KiB (8×64 sectors) to 0x10000 | kernel exceeds ~256 KiB, or moving the kernel off low memory |
| **virtio: MSI-X + multi-request** | INTx; one in-flight request **per device**, multi-disk via per-device serialization (ADR-015) | APIC live; concurrent in-flight I/O (request-tag table) |
| **Concurrent multi-thread block I/O** | self-tests share the block layer but the driver is single-in-flight-per-disk; per-mount FS lock not yet added | many threads issuing FS/block I/O at once (per-mount + block-layer locks, ADR-016) |
| **SMP spinlocks** | single-core; PMM/console/scheduler use interrupt masking (ADR-016) | second CPU brought online (APIC) |
| **SOVEREIGN FS (SFS) engine** | format + mount + empty-root only (ADR-018 slice 1) | B+ tree insert/lookup, file extents, journalled atomic tx, snapshots, 4 KiB tags, inline LZ4, free-space tree |
| **FAT32 LFN / timestamps** | 8.3 names; new entries get a zero date (no RTC) | long filenames; real mtimes (needs an RTC driver) |

See the per-item rows above for the primary (non-deferred) component status.
