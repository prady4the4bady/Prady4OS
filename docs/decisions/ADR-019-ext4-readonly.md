# ADR-019: ext4 read-only compatibility

- **Status:** Accepted 2026-06-18
- **Phase:** 4 (slice 4j, part 1)

## Context

PRADYOS needs to read existing Linux media (USB sticks, dual-boot partitions,
build artifacts) without the user reformatting to SFS. ext4 is the dominant
Linux filesystem. A read-only driver is enough for import/interop; writing ext4
is out of scope (SFS is the native writable store).

## Decision

A read-only ext4 driver (`kernel/fs/ext4/`) registered on the VFS mount table:

- **Mount** reads the superblock at byte offset 1024 (LBA 2), checks magic
  `0xEF53`, and caches geometry: block size (`1024 << s_log_block_size`),
  inodes-per-group, inode size, 64-bit feature + group-descriptor size, and the
  group-descriptor table block. Block sizes up to 4 KiB are supported (the
  scratch buffers are single pages).
- **Inode read**: group = (ino-1)/inodes_per_group, index = remainder; the group
  descriptor gives the inode-table block (64-bit hi half honored); the inode is
  read at `table*bs + index*inode_size`.
- **Extent mapping**: depth-0 extent trees (the inode's `i_block` holds an
  `ext4_extent_header` + up to 4 leaf extents), with 64-bit physical block
  numbers and uninitialized-extent length decoding.
- **Directory**: linear scan of `ext4_dir_entry_2` records across the directory's
  mapped blocks; absolute paths resolve component-by-component from the root
  inode (2). `open`/`read`/`readdir` are provided; `create`/`write`/`unlink`/
  `txn` are NULL (read-only).

Test images are built with `mkfs.ext4 -b 4096 -d <dir>` (populate at mkfs time,
no loop mount), forcing 4 KiB blocks to match the kernel's page-sized reads.

## Consequences / deferred

- **Read-only.** No ext4 writes (by design).
- **Depth-0 extents + extent-mapped inodes only.** Multi-level extent index
  nodes (very large/fragmented files) and legacy block-mapped (non-extent)
  inodes are deferred (return unmapped). Adequate for typical import.
- **No journal recovery, no htree dirs, no xattrs/ACLs.** Linear directory scan
  (htree falls back to linear, which is valid).
- Block sizes >4 KiB unsupported (single-page scratch).

## Verification

QEMU q35 with a 4th virtio-blk disk holding an ext4 volume (`mkfs.ext4 -b 4096
-d`, containing `/EXT4.TXT`): the kernel mounts it via the VFS probe (FAT32/SFS
decline, ext4 claims `0xEF53`) — four filesystems' disks coexist (FAT32, SFS,
ext4) — and reads the file back: `[ext4] /EXT4.TXT: "ext4 read works"`. Gate
`make smoke-fs-ext4`; CI runs it (installs e2fsprogs). `-Werror` clean.
