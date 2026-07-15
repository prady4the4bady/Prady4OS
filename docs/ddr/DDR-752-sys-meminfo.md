# DDR-752 — `SYS_MEMINFO` + PMM total-RAM tracking + PRISM `free`

**Status:** proposed (pre-code)
**Layer:** mm + syscall + user (PRISM). Closes the DDR-748 total-RAM deferral.

## Problem

`SYS_SYSINFO` (DDR-748) reports only *free* frames — total RAM was explicitly
deferred because the PMM tracked only the live free count. So ring 3 cannot show
memory *used* vs *total*, and there is no `free`/`meminfo`. The gap is a single
missing counter: the buddy allocator knows exactly how many frames it manages.

## Decision

**PMM total tracking (`kernel/mm/pmm.c`).** Capture `total_pages = free_pages`
once at the end of `pmm_init`'s E820 sweep — after every usable region is added
to the buddy pool, but *before* the permanent COW-refcount-table carve — so it is
the true managed-frame total. New getter `pmm_total_page_count()`. `used = total
− free` is derived, never stored (no drift).

**`SYS_MEMINFO` (NSI 74)** — `(struct meminfo *out) -> 0 | -EFAULT`. No
capability (non-sensitive, like the other introspection calls). Fills:

```c
struct meminfo {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;   /* total - free */
    uint32_t page_size;    /* 4096 */
    uint32_t _pad;
};
```

**PRISM `free`.** Prints `mem: total=<K>K free=<K>K used=<K>K` (KiB =
`pages * 4096 / 1024 = pages * 4`). Added to `help` + dispatch.

## Gate — extend `smoke-shell` (no new gate; stays 89)

Feed `free` before `exit` and assert the line matches
`mem: total=[0-9]+K free=[0-9]+K used=[0-9]+K` — proving `SYS_MEMINFO` round-tripped
with a sane shape. The exact figures depend on the E820 map but are stable per
boot; the shape is fixed, so the gate is deterministic. (No freestanding probe /
new gate — PRISM is the consumer, per the DDR-751 direction.)

## Non-goals

- No slab/kheap breakdown, no per-zone or per-order free lists, no high-water
  marks — just total/free/used frames.
- `used` counts *all* non-free frames (kernel image, page tables, the refcount
  table, every allocation) — it is physical-frame accounting, not a
  userspace-RSS figure.
- No `SYS_SYSINFO` struct change (its `free_pages` stays; `SYS_MEMINFO` is the
  richer, separable memory view — avoids resizing an already-mirrored struct).
