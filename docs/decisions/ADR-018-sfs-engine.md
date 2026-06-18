# ADR-018: SOVEREIGN Filesystem (SFS) engine — design + phased bring-up

- **Status:** Accepted 2026-06-18 (design); slice 1 implemented
- **Phase:** 4 (slice 4d onward)

## Context

FAT32 (ADR-015) is the interop "starter" filesystem. SFS is PRADYOS's native
store and the blueprint's real Layer-4 goal: a copy-on-write (CoW) B+ tree
filesystem with versioned snapshots, journalled atomic transactions, 4 KiB
metadata tags, and inline LZ4 compression. The on-disk structs and a declining
mount stub already exist (ADR-017). This ADR pins the engine design and the
order it is built in, so each slice is small and independently verifiable —
filesystem writers are corruption-prone, so we bring SFS up incrementally with a
host-checkable invariant at every step.

## Design

- **Block size 4 KiB** (8 × 512-byte sectors). One B+ tree node, one superblock,
  and one metadata tag each occupy a block.
- **Superblock (block 0):** magic/version, geometry, the current B+ tree root
  block, the latest committed snapshot root, the free-space tree root, the
  transaction-log extent, a monotonically increasing `generation`, and (until
  the free-space tree exists) a high-water `next_free_block` allocator.
- **Copy-on-write B+ tree:** keyed by `(object-id, offset)`. An update never
  overwrites a live node — it writes new node blocks and rewrites the path to a
  new root. Committing = atomically publishing the new root in the superblock
  (single-block write). This gives crash-consistency and cheap snapshots for
  free (an old root is a complete, immutable view).
- **Atomic transactions / journal:** a redo log in the `txn_log` extent. A
  transaction appends its new blocks, then commits by writing the new root +
  bumped generation to the superblock; replay on mount rolls forward any
  committed-but-not-yet-checkpointed log.
- **Versioned snapshots:** retain prior roots (by generation) so a snapshot is
  just a pinned root; garbage-collect unreferenced CoW blocks later.
- **Inline LZ4:** leaf extents may be stored LZ4-compressed, flagged per extent.
- **Capabilities:** SFS-native operations gated by `CAP_FS_SFS_READ/WRITE/ADMIN`
  (reserved in ADR-017), distinct from the generic `CAP_FS_*`.

## Phased bring-up (each slice smoke-verified)

1. **Format + mount + empty root (this slice).** In-kernel `sfs_format` writes
   the superblock + an empty root leaf; `sfs_mount` validates the superblock and
   caches geometry; `readdir` of the empty root returns no entries. Proves the
   on-disk format end-to-end (kernel formats a blank disk, kernel mounts it) and
   that the VFS mount table carries SFS alongside FAT32.
2. **B+ tree insert/lookup** → `create`/`open` (directory entries in the tree).
3. **Leaf extents** → `read`/`write` of file data (CoW, no journal yet).
4. **Journal + atomic commit** (crash-consistent multi-block updates).
5. **Snapshots** (retain + mount prior roots).
6. **Inline LZ4**; then free-space B+ tree (replace the high-water allocator);
   then garbage collection of stale CoW blocks.

## Consequences / deferred

- Slices 2-6 are deferred and tracked in build_status. Until the free-space tree
  exists, allocation is a non-reclaiming high-water mark (fine for bring-up).
- B+ tree node fan-out is provisional (`SFS_BTREE_FANOUT`); a node currently
  uses part of its 4 KiB block, to be tuned when inserts land.
- No SMP locking (single core; ADR-016 interrupt masking applies).

## Verification (slice 1)

QEMU q35 with a blank third virtio-blk disk: the kernel `sfs_format`s it, then
`vfs_mount` probes and SFS claims it (FAT32 declines, SFS magic matches); the
root directory mounts and lists empty. The volume coexists with the mounted
FAT32 disk (multi-FS mount table, ADR-017). smoke gate asserts the SFS mount
line; `-Werror` clean.
