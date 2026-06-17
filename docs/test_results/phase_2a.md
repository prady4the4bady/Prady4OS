# Phase 2a — NEXUS Kernel Core — Test Results

## Slice 1: Long-mode kernel entry (boot → 64-bit ring-0 C)

- **Date:** 2026-06-17
- **Environment:** WSL2 Ubuntu-22.04; clang 14, NASM 2.15.05, ld.lld 14,
  llvm-objcopy 14; QEMU 6.2.0 (SeaBIOS); AMD host.
- **New artifacts:** `arch/x86_64/boot.asm` (64-bit entry stub), `kernel/main.c`
  (`kmain`), `kernel/kernel.ld` (flat link at 0x10000). Stage 2 extended to load
  the kernel, build 4-level page tables, and enter long mode (ADR-005).

### Commands

```bash
make kernel   # nasm entry + clang main, link flat at 0x10000, objcopy to raw
make image    # stage1 (LBA0) + stage2 (LBA1) + kernel (LBA17) on a 1 MiB disk
make smoke    # boots, greps COM1 for the kernel sentinel NEXUS KERNEL OK
```

### Expected vs actual

| Check | Expected | Actual |
|---|---|---|
| `kernel_entry` address | 0x10000 (load addr) | **PASS** — `llvm-nm`: `0000000000010000 T kernel_entry` |
| kernel size | <= 32 KiB | **PASS** — 519 bytes |
| `make smoke` (sentinel `NEXUS KERNEL OK`) | exit 0 | **PASS** |

### Actual serial output (QEMU)

```
PRADYOS S1: loading stage2...
PRADYOS S2: protected-mode loader
  A20: enabled (fast, port 0x92)
  E820 map entries (hex): 0x06
  CPU vendor: AuthenticAMD
  Long mode supported: yes
  loading kernel @0x10000...
  switching to protected mode...
PRADYOS BOOT OK
PRADYOS S2: enabling long mode, jumping to NEXUS...
NEXUS: entered kmain (64-bit long mode, ring 0)
NEXUS KERNEL OK
```

This confirms: Stage 2 loads the kernel via INT 13h; the 32→long-mode transition
(PAE + identity-mapped 1 GiB + EFER.LME + CR0.PG + far jump to an L-bit CS)
works; and the C `kmain` executes in 64-bit ring 0.

### Notes / what is NOT done yet (see ADR-005, build_status)

- Kernel runs on the **bootloader's** GDT with **no IDT** — any CPU exception
  triple-faults. Kernel-owned GDT + IDT + exception handlers are the next slice.
- **No hardware-info handoff**: Stage 2's E820/CPUID data is not yet passed to
  the kernel. Defining that struct/ABI is the next slice (closes the Phase 1
  kernel-handoff item).
- Identity-mapped low 1 GiB only; flat kernel at 0x10000. Higher-half + real VMM
  + ELF loading are Phase 2b.
- `make smoke` still waits the 30 s QEMU watchdog (kernel halts; no self-exit).
- VirtualBox not yet exercised.
