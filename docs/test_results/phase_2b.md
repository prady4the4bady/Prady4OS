# Phase 2b — Memory Management — Test Results

## Slice 1: Physical memory manager (buddy allocator)

- **Date:** 2026-06-17
- **Decision:** ADR-003 — buddy allocator (user-approved over the Blueprint's PFO).
- **Files:** `kernel/pmm.{c,h}`. Orders 0..10 (4 KiB .. 4 MiB blocks), intrusive
  per-order free lists, split on alloc, XOR-buddy coalesce on free. Seeded from
  the boot_info E820 map; manages usable RAM in [16 MiB, 1 GiB).

### Commands

```bash
make image && make smoke    # smoke PASS (sentinel NEXUS KERNEL OK)
```

### Actual self-test output (QEMU, 128 MB)

```
NEXUS: PMM (buddy) free frames=0x0000000000006FE0
  alloc 1 frame  -> 0x0000000007FC0000
  alloc 8 frames -> 0x0000000007FC8000
  free frames after alloc=0x0000000000006FD7
  free frames after release=0x0000000000006FE0  (balanced)
```

Interpretation:
- `0x6FE0` = 28640 frames ≈ 111.9 MiB — matches the managed window
  [16 MiB, ~127.875 MiB) (the usable E820 region clipped at 16 MiB).
- order-0 alloc is page-aligned; order-3 (8 frames) alloc is 32 KiB-aligned —
  correct natural alignment per order.
- after alloc, free = 28640 − (1 + 8) = 28631 (`0x6FD7`) ✓.
- after release, free returns to `0x6FE0` — coalescing restored the exact count;
  no leak.

### Notes / not done

- Buddy lookup on free is a linear scan of the order's free list (correct;
  a per-block state bitmap is the obvious later speedup).
- Managed region capped at [16 MiB, 1 GiB) because only the low 1 GiB is
  identity-mapped (ADR-005) and free-list links live in the frames. Lifts once
  the VMM maps more.
- Next: VMM (kernel-built 4-level paging; higher-half kernel decision per
  ADR-005).

## Slice 2: kernel heap (slab + kmalloc)

- **Date:** 2026-06-17
- **Files:** `kernel/kheap.{c,h}`, `kernel/string.{c,h}` (freestanding mem*).
- **Design:** one PMM page per slab with a header at offset 0 (so slab objects
  are never page-aligned → kfree distinguishes them from whole-page large
  allocs). kmalloc ≤ 2 KiB → size-class slab caches (16..2048); larger → whole
  pages from the PMM tracked in a registry. Dedicated caches for PCB / capability
  token / IPC message; page-table nodes are zeroed full pages from the PMM.
- **Debug (KHEAP_DEBUG):** free-poisoning (0xDD), double-free detection (free-list
  scan), bad-pointer/magic checks, and per-pool leak accounting via
  `kheap_outstanding()`.

### Verified (QEMU)

```
NEXUS: kheap stress — outstanding base=0x0 after=0x0  (no leak)
```

Stress test: 64 mixed-size kmalloc/kfree (slab + large) with first/last-byte
writes, plus pcb/cap/ipc/ptnode alloc+free. Outstanding returns to 0 — no leak,
no double-free/poison trip. smoke PASS.

### Build hardening

- `-Werror` now enforced for **both** clang (`-Wall -Wextra -Werror`) and NASM
  (`-Werror`) across the kernel and bootloader. Any warning fails the build
  (user mandate: zero warnings at any point). Full clean rebuild is warning-free.

### Notes / not done

- Slab free-slab pages are not returned to the PMM when fully empty (kept for
  reuse); a reclaim pass is a later optimization.
- Buddy/large lookups and slab-with-free search are linear; fine for now.
- Next: VMM (4-level paging owned by the kernel; higher-half per ADR-005).

## Slice 3: higher-half kernel + kernel-owned VMM (ADR-007)

- **Date:** 2026-06-18
- **Files:** `kernel/kernel.ld` (relink to 0xFFFFFFFF80000000, LMA 0x10000,
  __bss bounds), `arch/x86_64/boot.asm` (high stack + .bss zero + RDI preserve),
  `boot/stage2/stage2.asm` (build higher-half map + low identity, jump to
  KERNEL_VIRT), `kernel/vmm.{c,h}`, Makefile (`-mcmodel=kernel`).

### Verified (QEMU)

- **Higher-half execution:** `int3` self-test now reports
  `RIP=0xFFFFFFFF8000026D`; `llvm-nm` shows `kernel_entry @ ffffffff80000000`.
  All prior self-tests (GDT/IDT/timer/PMM/heap) pass from higher-half, using the
  retained low identity map to reach physical RAM.
- **Kernel-owned VMM:**
  ```
  NEXUS: vmm_map va=0xFFFF800000000000 pa=0x07F89000 readback=0xCAFEBABEDEADBEEF  (OK)
  NEXUS: vmm unmap reclaim — outstanding 0x0 -> 0x0  (clean)
  ```
  vmm_map allocated PDPT/PD/PT frames from the PMM, mapped a fresh page into an
  empty PML4 slot, and the readback matched; vmm_unmap cleared the PTE and
  reclaimed all three now-empty tables (via ptnode_free) leak-free.
- smoke PASS; full `-Werror` build is warning-free.

### Notes / deferred (ADR-007)

- Low identity map kept so PMM/heap need no changes; a physmap + identity drop +
  per-process CR3 come with user address spaces.
- NX/W^X (`VMM_NX` defined, unused) needs EFER.NXE + per-section permissions —
  a later hardening slice.

**Phase 2b (memory management) is complete:** buddy PMM + slab heap + higher-half
kernel-owned 4-level paging, all self-tested.
