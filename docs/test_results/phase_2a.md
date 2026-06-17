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

## Slice 2: Kernel GDT + IDT + CPU exception handlers

- **Date:** 2026-06-17
- **New artifacts:** `arch/x86_64/cpu.asm` (kernel GDT + `gdt_init` + `idt_load`),
  `arch/x86_64/isr.asm` (32 exception stubs + `isr_common` + `isr_stub_table`),
  `kernel/idt.c` (`idt_init`, `isr_dispatch`), `kernel/console.{c,h}` (shared
  serial/VGA/hex). `kmain` now loads the GDT, installs the IDT, and runs an
  `int3` self-test. Built `-fno-omit-frame-pointer` for backtraces.

### What was verified (QEMU, AMD host)

1. **Kernel GDT** installed; CS reloaded to 0x08 via far return (confirmed: the
   panic dump below shows `CS=0x08`).
2. **IDT** with all 32 CPU exception vectors loaded.
3. **Recoverable exception** — `int3` (#BP) is caught and execution resumes:
   `[#BP] breakpoint at RIP=0x...101BE — IDT works, resuming` then the kernel
   continues. `make smoke` PASS (sentinel printed before and the kernel runs on).
4. **Panic path** — verified once with a throwaway `ud2` (#UD), **not committed**.
   The handler printed (per the error-handling mandate):

   ```
   *** NEXUS KERNEL PANIC ***
   component: NEXUS isr
   exception: #UD invalid opcode  vector=0x...06  error=0x...0
   RIP=0x...101CD  CS =0x...08  RFLAGS=0x...02  RSP=0x...1FFFF0
   RAX=... .. R15=...            (full GP register dump)
   backtrace:
     0x...1000D                  (= kernel_entry.hang, the kmain return addr)
   halting.
   ```

   This confirms exception identification, the register dump, the frame-pointer
   backtrace, and a clean halt. CR2 is additionally printed for vector 14 (#PF).

### Not done yet (as of slice 2)

- **No TSS** (handled in ring 0, no IST, no privilege change). TSS + IST arrive
  with ring-3 / double-fault-stack support.

## Slice 3: boot→kernel handoff struct

- **Date:** 2026-06-17. Added `kernel/boot_info.h`; Stage 2 fills the struct at
  phys 0x4000 (magic, E820 entries, count, CPUID vendor, long-mode flag) and
  passes the pointer in RDI. `kmain` validates the magic and prints the map.
- **Verified (QEMU):** kernel printed the 6-entry 128 MB QEMU E820 map and
  `vendor=AuthenticAMD long_mode=0x1` straight from the handoff struct. Closes
  Phase 1's last blocking item.

## Slice 4: legacy PIC + PIT timer + keyboard IRQ

- **Date:** 2026-06-17. Added `kernel/io.h` (shared port I/O), `kernel/irq.{c,h}`
  (PIC remap/EOI, PIT). Extended `isr.asm` to vectors 32..47 and the IDT to 48
  gates; `isr_dispatch` branches IRQ vs exception and sends EOI. `kmain` remaps
  the PIC, programs the PIT to 100 Hz, runs `sti`, and waits for ticks.
- **Decision:** legacy 8259 PIC + 8254 PIT now; APIC deferred to Phase 2b
  (ADR-006).
- **Verified (QEMU):**
  ```
  NEXUS: PIC remapped, PIT @100Hz; enabling interrupts (sti)
  NEXUS: timer IRQ alive, ticks=0x0000000000000005
  NEXUS: idle (halt, interrupts on)
  ```
  IRQ0 fires; the kernel wakes from `hlt` and counts ticks — the full interrupt
  pipeline (PIC → IDT gate → stub → dispatch → EOI → iretq) works. smoke PASS.
- **Not done / notes:** keyboard IRQ1 handler installed but not exercised in the
  headless smoke (no key input); spurious IRQ7/IRQ15 not specially handled yet
  (ADR-006). APIC, PCB, context switch, and the scheduler are next.
