# DDR-764 — enlarge the ring-3 VFS write chunk (256 B → 4 KiB block)

**Status:** proposed (pre-code)
**Layer:** syscall (sys_io). M2 storage — makes the persistent root usable for writes.

## Problem

`fd_write_user` (the `SYS_WRITE`/`SYS_WRITEV` path for a `FD_VFS` file, sys_io.c)
copies user data through a **256-byte** kernel buffer, calling `vfs_write` once per
256 bytes. Two real consequences for ring-3 file writes:

- **Throughput:** an N-byte write does `N/256` copyin + `vfs_write` (+ FS
  write-and-verify) iterations — 16× more than a one-block chunk.
- **SFS capacity (severe):** each 256-byte `vfs_write` becomes one SFS *extent*,
  and an SFS inode holds at most 4 inline extents. So a ring-3 process can write
  **only ~1 KiB** to an SFS file before the 5th extent is rejected (`sfs_write`
  returns short). The kernel's own writes don't hit this — they call `vfs_write`
  once with the full length (one extent) — but ring 3 can't.

The kernel writes 64 KiB to SFS fine (the free-space-GC/churn self-tests), so this
is purely the ring-3 chunking, not an FS limit.

## Decision

Raise the `FD_VFS` write chunk from 256 B to **one 4 KiB block**, using a
**PMM page** as the bounce buffer (not a stack array — the kernel stack is 16 KiB
and a 4 KiB frame is too much; one `pmm_alloc_page`/`pmm_free_page` per
`fd_write_user` call covers the whole write). The loop is otherwise unchanged
(chunked copyin → `vfs_write` → advance offset; short/partial writes still return
the byte count).

Effect: 16× fewer iterations for any ring-3 VFS write, and ring-3 **SFS** files
grow to **4 × 4 KiB = 16 KiB** (4 extents of one block each) — 16× the old 1 KiB.
FAT32 (no extent cap) simply writes 16× faster. The console/pipe write paths are
untouched (they keep their own small buffers).

Not solved here (documented follow-ons): SFS's 4-extent inode cap still bounds a
ring-3 SFS file at 16 KiB — lifting it needs an extent-overflow/indirect-extent
tree (a later SFS slice); and the 1 MiB per-thread `fs_write_budget` still caps
lifetime bytes (a refillable-budget review is a separate flagged slice).

## Gate — `smoke-vfs-bigwrite` (new; 98 → 99)

A freestanding probe rooted at the SFS root creates `/BIG.TXT`, writes **8 KiB**
in one `SYS_WRITE`, closes, re-opens, reads it back, and verifies the bytes. With
the old 256 B chunk the write returns short at ~1 KiB (5th SFS extent rejected) →
readback mismatch → fail; with the 4 KiB chunk the 8 KiB lands in 2 extents →
`PRADYOS_BIGWRITE_OK`. Spawned like the DDR-760 SFS-root probe (its `root_mnt` set
to the persistent SFS root). Assert the sentinel; `BIGWRITE FAIL` forbidden.

## Non-goals

- No SFS extent-overflow (the 4-extent cap stays; 16 KiB ceiling for now).
- No write-budget change (separate slice).
- No change to console/pipe write buffering.
