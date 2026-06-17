# ADR-005: Long-mode bring-up and kernel load format (Phase 2a)

- **Status:** Accepted (provisional — revisit at Phase 2b VMM)
- **Date:** 2026-06-17
- **Phase:** 2a

## Context

Phase 2a needs the bootloader to hand control to a 64-bit NEXUS kernel. Several
implementation details were left open by the source documents and had to be
chosen to make a minimal, testable slice. None of these are deviations from the
blueprint's *architecture* — they fill in unspecified mechanics — so they are
self-answered here per the Instructions' "self-answer all gaps" rule rather than
escalated as architectural forks.

## Decisions

1. **Early identity map, not higher-half (yet).** Stage 2 builds a 4-level page
   table that identity-maps the low 1 GiB with 2 MiB pages (PML4[0] → PDPT[0] →
   PD of 512 × 2 MiB), then enables PAE → CR3 → EFER.LME → CR0.PG and far-jumps
   to a CS with the L bit set (Intel SDM Vol.3 9.8.5). Identity mapping is the
   universal bring-up approach and does not preclude a higher-half kernel later.

2. **Kernel loaded as a flat binary at physical 0x10000.** Real-mode INT 13h
   cannot write at/above 1 MiB, and we have no ELF parser yet. So the kernel is
   linked flat (`kernel/kernel.ld`, base 0x10000, `kernel_entry` first via a
   `.text.boot` section), objcopied to a raw binary, placed at LBA 17 of the
   disk image, loaded by Stage 2 to 0x10000, and entered directly.

3. **Kernel built with `-mgeneral-regs-only`.** SSE/AVX are not enabled in CR0/
   CR4 during bring-up, so the compiler must not emit SIMD. This avoids a #UD
   before we set up FPU/SSE state (a later slice).

4. **Page tables at 0x70000–0x73000, kernel stack at 0x200000.** Fixed low-RAM
   addresses chosen to avoid the loaded image (Stage 2 ~0x7E00, kernel ~0x10000),
   the VGA window (0xB8000), and the EBDA (~0x9FC00). All identity-mapped.

## Consequences / what this defers

- **Higher-half kernel + real VMM:** Phase 2b. The kernel virtual base will move
  (conventionally 0xFFFFFFFF80000000) and paging will be rebuilt properly.
- **ELF loading + kernel relocation to 1 MiB+:** deferred. The flat 0x10000 base
  is provisional and capped at 32 KiB (Stage 2 loads 64 sectors).
- **Hardware-info handoff struct:** Stage 2 gathers E820 + CPUID but does not yet
  pass them to the kernel. Defining that ABI is the next slice (it closes Phase
  1's remaining kernel-handoff item).
- **No IDT/GDT-in-kernel yet:** the kernel runs on the bootloader's GDT with no
  interrupt handling. Any fault triple-faults. IDT + exception handlers are the
  next Phase 2a slice.

## Alternatives considered

- **UEFI boot (skips manual long-mode setup; firmware starts in long mode).**
  Deferred: the user chose the BIOS/MBR path first, and the manual transition is
  exactly the assembly-first work Phase 2a calls for.
- **Load kernel ELF and parse program headers in Stage 2.** More correct but
  larger; not justified for a single-segment flat kernel this early. Adopt when
  the kernel grows past a flat layout (segments, BSS that matters, relocation).
