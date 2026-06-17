# ADR-007: Higher-half kernel + kernel-owned VMM

- **Status:** Accepted 2026-06-18 (user-approved). Supersedes the "provisional
  low/flat" parts of ADR-005.
- **Phase:** 2b

## Context

ADR-005 brought the kernel up flat at physical 0x10000, identity-mapped, and
explicitly deferred the higher-half decision to "when the VMM lands." It has
landed. Doing higher-half now is cheapest: the kernel is ~600 lines, so every
later subsystem can assume the final address model instead of being retrofitted.

## Decision

1. **Higher-half kernel at `0xFFFFFFFF80000000`** (the conventional -2 GiB kernel
   window; matches `-mcmodel=kernel`). `kernel/kernel.ld` links the kernel at this
   VMA with LMA = physical 0x10000 (`AT(...)`), so `objcopy -O binary` still
   produces a flat image loaded at 0x10000.

2. **The bootloader establishes the higher-half mapping and jumps high.** Stage 2
   builds, in addition to the low 1 GiB identity map, a 4 KiB-page mapping of
   `0xFFFFFFFF80000000.. -> 0x10000..` (PML4[511]->PDPT[510]->PD[0]->PT, 2 MiB
   span) and `jmp`s to `KERNEL_VIRT`. The kernel runs at higher-half from its
   first instruction — no fragile position-independent early-C trampoline.

3. **The low identity map is kept (for now).** The PMM and heap dereference
   physical frames directly through it, so they need no changes. A proper physmap
   + dropping the identity map is deferred to when per-process address spaces
   arrive (it becomes necessary then, not before).

4. **Kernel-owned VMM** (`kernel/vmm.{c,h}`): `vmm_map`/`vmm_unmap` walk the
   active tables (rooted at CR3), allocate intermediate tables from the PMM
   (`ptnode_alloc`), and reclaim emptied tables on unmap (`ptnode_free`), keeping
   heap leak-accounting consistent. Tables are reached via the identity map.

5. **Entry stub zeroes `.bss`.** `.bss` is NOLOAD (absent from the flat image),
   so `kernel_entry` zeroes `[__bss_start, __bss_end)` before calling `kmain`, and
   switches to a `.bss` kernel stack. RDI (boot_info) is preserved across this.

## Consequences / deferred

- **NX/W^X**: `VMM_NX` is defined but unused (needs EFER.NXE + per-section perms).
  A later hardening slice sets X only on .text, RW-NX on data, RO on .rodata.
- **Physmap + identity drop + per-process CR3**: deferred to the process/user-space
  work; the kernel keeps a single shared address space until then.
- **Bootloader-built top-level tables**: the kernel edits them in place and can
  build sub-tables from the PMM; a full rebuild of the root tables from PMM at
  kernel init is a possible later cleanup but not required for correctness.

## Verification

QEMU: `int3` reports `RIP=0xFFFFFFFF8000026D` (executing higher-half); all prior
self-tests pass from higher-half; `vmm_map` of a fresh frame at an unused PML4
slot reads back correctly and `vmm_unmap` reclaims tables leak-free. smoke PASS,
`-Werror` clean.
