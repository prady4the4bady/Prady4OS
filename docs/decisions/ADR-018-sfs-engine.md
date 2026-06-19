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

## On-disk format (slice 2, BINDING — durable, do not change without migration)

Inode-based, 4 KiB blocks. The B+tree is keyed by sorted `uint64`:

- **DIR**    `(parent_inode << 32) | name_hash32(name)` — value: `inode_num` +
  `name_len` + `name[255]`. Maps a name in a directory to an inode number
  (hardlink-ready: many DIR keys may share one `inode_num`).
- **INODE**  `0x8000000000000000 | inode_number` — value: the **inode block
  pointer**. (Indirection is required: CoW relocates the inode block on every
  update, so the tree entry is repointed at commit.)
- **EXTENT** `0x4000000000000000 | (inode << 20) | block_idx` — value:
  `block_start` + `block_count` (for files exceeding the 4 inline extents).

**Resolution of an inconsistency in the spec:** an inode record carries a ~4 KiB
metadata tag, so it is 4182 B with the header — larger than a 4096 leaf slot.
The inode therefore lives in its **own 4096-byte block** (header ~100 B +
tag ~3996 B), and the INODE-keyed leaf entry stores only the block pointer.
Every field from the spec is preserved; only the tag's residence moves from
"inline in the leaf" to "the inode's own block," which is also what CoW needs.

- **Node (4096 B):** header `{flags, nkeys, next_leaf, generation}` then either
  leaf slots (272 B: `key` + union{dir | inode_ptr | extent}) or internal slots
  (`child[0]` then `nkeys × {separator_key, child_block}`).
- **Inode block (4096 B):** `size, flags, ctime, mtime, extent_count,
  inline_extents[4], tag[~3996]`.
- **Superblock adds (version 2):** `root_btree`, `journal_start/len`,
  `next_inode`, `next_free_block`, `free_block_count`.
- **CoW insert:** never mutate a live node. Build new node blocks bottom-up
  (split on overflow), allocate from the high-water `next_free_block`, and
  **commit by writing the superblock** with the new root + bumped generation —
  a single-block atomic publish (the journal in slice 4 makes multi-block
  metadata updates crash-atomic too).

Leaf fan-out is 14 (uniform 272 B slots) for slice 2 — correctness first;
typed/compacted leaves are a later optimization. The 4 inline extents keep small
files off the EXTENT keyspace entirely.

## Phased bring-up (each slice smoke-verified)

1. **Format + mount + empty root (slice 4d).** In-kernel `sfs_format` writes
   the superblock + an empty root leaf; `sfs_mount` validates the superblock and
   caches geometry; `readdir` of the empty root returns no entries. Proves the
   on-disk format end-to-end and that the VFS mount table carries SFS alongside
   FAT32.
2. **B+ tree insert/lookup → `create`/`open` (slice 4e, done).** Copy-on-write
   insert: split-on-overflow, never mutating a live node; the leaf→root path is
   rebuilt into new blocks and the new root published by the superblock write.
   `create` allocates an inode block + inode number and inserts an INODE entry
   and a DIR entry; `lookup` resolves a name to an inode number (with a name
   re-check to guard hash collisions); `readdir` enumerates by in-order tree
   walk (not the `next_leaf` chain, which CoW splits leave stale). Verified by
   creating 10 files (forces a leaf split + a 2-level tree), looking each up,
   and enumerating all 10. Full internal-node split is implemented but only
   exercised at large directory scale.
3. **Leaf extents → `read`/`write` (slice 4f, done).** `write` (append/grow:
   `off == size`) allocates a contiguous extent from the high-water allocator,
   writes the data blocks, records the extent in the inode's 4 inline-extent
   slots, CoWs the inode to a new block, and repoints its INODE entry (the
   replace-on-equal-key insert). `read` walks the inline extents in order and
   copies the requested byte range, clamped to file size. Verified by writing
   64 KiB, reading it back byte-exact, then growing past EOF and confirming the
   new size. Mid-file overwrite and >4 extents (EXTENT-keyed spill) are a later
   sub-slice; the 4 inline extents keep small files off the EXTENT keyspace.
4. **Journal + atomic commit (slice 4g, done).** Transactions batch operations
   by deferring the superblock write: `txn_begin` snapshots the rollback point,
   ops accumulate CoW blocks + a new in-memory root, `txn_commit` writes a
   CRC-protected journal commit record (the intended root + counters) *then* the
   superblock, and `txn_abort` reverts the in-memory root (uncommitted blocks
   are simply forgotten — `next_free` rolls back). **Design note:** because SFS
   is copy-on-write, blocks are never overwritten in place, so the spec's
   physical "shadow-block" journal is unnecessary — a *logical* commit-record
   that makes the root-swap atomic is the correct CoW equivalent. On mount, if
   the journal holds a CRC-valid record with `txn_id > superblock.generation`
   (the commit's superblock write was lost to a crash), it is **replayed**.
   Verified end-to-end (`sfs_selftest_journal`) across real mount/unmount
   cycles: abort discards, commit persists, and a simulated torn commit
   (journal written, superblock lost) is recovered on remount.
5. **Snapshots (slice 4h, done).** A snapshot is simply a retained B+ tree root
   captured at a generation (stored in a 16-entry table in the superblock).
   Because SFS is copy-on-write and the high-water allocator never reuses blocks,
   the captured root and everything reachable from it stay valid with no extra
   work. `sfs_snapshot` records `{id=generation, root}`; `sfs_open_version` reads
   a file as of a snapshot by resolving it against the snapshot's root (a
   versioned `vfs_file` carries that root in `dirent_clus`; writes to it are
   refused). Verified: write v1, snapshot, grow to v2, then read v1 through the
   snapshot and confirm it is byte-unchanged while the current view shows v2.
   **Deferred (own sub-slice):** the free-space B+ tree and snapshot GC — the
   high-water allocator is correct but non-reclaiming, so dropped snapshots'
   blocks are not yet returned. Reclamation needs the free-space tree + a
   snapshot-delete API + reference/mark-sweep; tracked in build_status. (This is
   a persisted-format addition, so it gets its own ADR review before bytes land.)
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
