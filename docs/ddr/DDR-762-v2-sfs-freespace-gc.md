# DDR-762-v2 — SFS free-space reclamation (free-extent-run allocator)

**Status:** proposed (pre-code). Supersedes the reverted DDR-762 (block-stack).
**Layer:** fs (SFS). M2 storage. Closes the DDR-741-deferred block leak.

## Problem

SFS's allocator is a pure high-water bump (`alloc_block` = `next_free++`); unlink
only tombstones the dir entry (DDR-741), so a file's inode + data-extent blocks
leak. A create/unlink loop grows `next_free` until the volume is exhausted. With a
persistent SFS root (DDR-760), that matters.

## Why the naive design is WRONG (learned in the reverted DDR-762)

A per-block LIFO free stack corrupts data: SFS extents are CONTIGUOUS runs, and
`write_extent` records `ext->block_start = start` where `start = c->next_free`,
assuming `alloc_block` returns consecutive blocks from the high-water mark.
Scattered freed blocks break that — data goes to freed blocks while the extent
points at `[next_free, next_free+n)`.

## Decision — free-EXTENT-RUN allocator (contiguity-preserving)

- `sfs_ctx` gains `struct { uint64_t start; uint32_t count; } free_runs[256];
  uint32_t free_run_count;` (`free_run_count` zeroed at mount — kmalloc doesn't
  zero).
- **`free_run(c, start, count)`** pushes the run **only when
  `c->snapshot_count == 0`** (no snapshot can reference it) and there is room;
  else it leaks (bounded — correctness over completeness).
- **`alloc_run(c, n)`** first-fit a reclaimed run with `count >= n`, split it
  (`run.start += n; run.count -= n`, swap-remove if emptied), return the start;
  else bump `next_free` by `n`. Returns `n` CONTIGUOUS blocks.
- **`alloc_block(c) = alloc_run(c, 1)`** (inode blocks, B+tree nodes).
- **`write_extent`** allocates its run with `alloc_run(c, nblocks)` and writes
  `[start, start+nblocks)` (contiguous).
- **`sfs_unlink`** (before tombstoning; files only): read the inode,
  `free_run(extent.block_start, extent.block_count)` per extent + `free_run(iblk,
  1)`. The snapshot guard inside `free_run` makes this a no-op when a snapshot
  exists.

**Correctness invariant.** A run enters `free_runs` ONLY when `snapshot_count == 0`
at that instant — no snapshot references it and it left the live tree. Snapshots
created later capture only the live tree. So a reused run is never
snapshot-referenced; contiguity is preserved by construction. Uniform-size files
(the common case) reuse a freed run exactly (no fragmentation). In-memory only
(within-a-boot reclaim); on-disk free tracking (`free_extent_tree`) is deferred.

## Gate — `smoke-sfs-gc` (new; 97 → 98)

Boot self-test on the reclaimed SFS root (`snapshot_count == 0`): **refresh the
boot thread's `fs_write_budget`** (DDR-763 lesson — else the 1 MiB budget caps the
loop at ~10, masking block reuse), then 300× { create `/GC.TMP`, write 64 KiB,
unlink }. Discriminating: without reclamation, ~300×16 data blocks exceed the
~4096-block volume and a `wr_block` past the disk fails; WITH the run allocator
each unlink's 16-block run is reused by the next write (exact fit) so all 300
succeed → `[sfs] free-space GC OK`. (Verified during development that disabling
`free_run` reproduces the pre-300 exhaustion — the reuse is real and necessary.)
Regression: full SFS suite + `smoke-sfs-btree` + `sfsroot` (no corruption).

## Non-goals

- No CoW superseded-version reclaim (overwrite/grow still leak old versions).
- No reclaim while snapshots exist (guarded off — correctness).
- No on-disk free list / cross-reboot persistence (host `mkfs.sfs` era).
- No inode-B+tree-entry removal (orphaned `SFS_KEY_INODE` slot leaks a small
  metadata entry, never a data block; bt has no delete).
