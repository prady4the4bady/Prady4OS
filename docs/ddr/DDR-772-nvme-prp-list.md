# DDR-772 — NVMe PRP2 + PRP list (multi-page single commands)

**Status:** implemented — `smoke-nvme` extended, PASS (`PRADYOS_NVME_PRP_OK`: a
16 KiB / 4-page transfer round-trips as ONE command via a 3-entry PRP list).
Extends DDR-766 (NVMe block I/O). NVMe IRQ is a separate follow-on (needs an
MSI-X vector — deferred).

## Problem

`nvme_io` (DDR-766) issues **one NVM command per ≤page chunk** — a PRP1-only
loop. A 16 KiB block request becomes 4 commands; a 128 KiB request, 32. NVMe
commands natively address multi-page transfers via **PRP2**: for a 2-page
transfer PRP2 is the second page; for >2 pages PRP2 points to a **PRP list**
(an array of page addresses). Using it collapses a large transfer to one (or few)
commands.

## Decision — `kernel/drivers/nvme/nvme.c`

1. `nvme_submit` gains a `prp2` argument (sets SQE `PRP2` at byte 32); the four
   `nvme_init` callers (Identify ×2, Create I/O CQ/SQ) pass 0.
2. Allocate one scratch **PRP-list page** per controller (`n->prp_list`) when the
   I/O queue comes up. One list page holds 512 × 8-byte entries → a single
   command covers PRP1 (a possibly page-offset first region) + up to 512 further
   pages ≈ 2 MiB.
3. Rewrite `nvme_io(is_write, lba, buf_phys, sectors)` to build PRPs per command,
   capped at what one list page covers (`NVME_MAX_LBAS = 4096` sectors = 2 MiB);
   loop for larger:
   - `off = buf_phys & 0xFFF`; `PRP1 = buf_phys`; first region = `PAGE_SIZE-off`.
   - transfer fits in PRP1 → `PRP2 = 0`.
   - one more page → `PRP2 = second_page_base`.
   - N>1 more pages → fill `prp_list[i] = second_page_base + i*PAGE_SIZE` and set
     `PRP2 = n->prp_list`.
   PRP-list entries are page-aligned (as NVMe requires); PRP1 may carry the offset.

Polling is unchanged (synchronous, one in-flight command → the single scratch
list page is safe to reuse per command).

## Gate — extend `smoke-nvme`

`nvme_selftest` adds a **16 KiB** (4-page) round-trip at a distinct LBA:
`pmm_alloc_pages(2)`, fill a pattern, `nvme_io` write then read (32 sectors, which
now takes ONE command via a 3-entry PRP list), verify byte-exact →
`PRADYOS_NVME_PRP_OK`. The gate asserts it alongside the existing 4 KiB
`PRADYOS_NVME_RW_OK`.

## Non-goals

- NVMe completion IRQ (still polled — an MSI-X vector + dispatch is a separate
  slice, and the shared MSI-X window is currently full at 50–63).
- Transfers > one PRP-list page in a single command (chained PRP lists) — the
  per-command cap loops instead.
- Unaligned-buffer micro-optimisation beyond the standard PRP1-offset handling.
