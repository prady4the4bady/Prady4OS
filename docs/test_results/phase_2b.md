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
  ADR-005), then a SLAB/slab-style kernel heap on top of the PMM.
