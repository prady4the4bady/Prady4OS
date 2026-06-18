# ADR-017: VFS mount table + per-mount context + SFS skeleton

- **Status:** Accepted 2026-06-18
- **Phase:** 4 (slice 4c)

## Context

Slice 4a shipped a single-mount VFS with the FAT32 driver keeping volume state
in file-scope globals. To support FAT32 writes cleanly, and to be ready for the
SOVEREIGN filesystem (SFS) and ext4 mounting alongside FAT32, the VFS needs (a)
per-mount state instead of globals and (b) more than one mounted volume at a
time. This slice also stands up the SFS on-disk layout and driver skeleton so
the rest of the system (capabilities, tooling, the VFS multi-FS path) can be
built against SFS before its engine exists.

## Decision

- **Mount table** (`kernel/fs/vfs/`): `vfs_mount(blk_index)` probes each
  registered driver, and on a match records a mount — `{fs ops, block device,
  per-mount context}` — in a small fixed table, returning a mount id. The
  context-based ops (`mount` allocates `*ctx`; every other op receives it) keep
  all volume state out of globals, so FAT32/SFS/ext4 volumes can be mounted
  concurrently. `open`/`create`/`unlink`/`readdir` take a mount id; `read`/
  `write` take a `vfs_file` that remembers its owning mount.
- **FAT32 per-mount context**: geometry + two scratch sectors live in a
  `kmalloc`'d `fat32_ctx`, one per mounted volume (replacing the old globals).
- **SFS skeleton** (`kernel/fs/sfs/`): the on-disk `sfs_superblock` and
  `sfs_btree_node` structs (copy-on-write B+ tree, versioning, transaction log,
  LZ4 feature bits) are pinned in `sfs.h`. The driver registers `sfs_ops` whose
  `mount` probe recognises no volume yet (always declines) and whose data ops
  return failure. So SFS occupies a registered slot and exercises the multi-FS
  probe path, while FAT32 remains the only concrete filesystem.
- **SFS capability classes** (`kernel/cap.h`): `CAP_FS_SFS_READ/WRITE/ADMIN`
  reserved up front, distinct from the generic `CAP_FS_*`, so SFS-native
  operations (snapshots, transactions, compaction) can be gated independently
  when the engine lands.

## Consequences / deferred

- **Single mount per call, no mount-point namespace.** The table supports
  several concurrent volumes by id, but there is no path prefix routing
  (`/mnt/x`) yet — a full mount-point namespace is deferred.
- **SFS engine is entirely future work**: copy-on-write B+ tree, versioned
  snapshots, journalled atomic transactions, 4 KiB metadata tags, and inline
  LZ4 are all deferred (tracked in build_status). This slice is layout + stubs
  only.
- ext4 read/write remains deferred; it will register on the same mount table.

## Verification

The VFS mounts FAT32 on a disk via the new mount table and returns a mount id;
all FAT32 read/write/readdir traffic flows through the per-mount context. The
SFS driver registers and its probe correctly declines the FAT32 disk (FAT32
still claims it). Build is warning-free `-Werror` (C + NASM); `make smoke` and
the FS gates pass.
