# DDR-767 — host `mkfs.sfs` (build-time SFS provisioning + persistence proof)

**Status:** implemented — `smoke-mkfs-sfs` PASS (host round-trip: mkfs writes
`/PERSIST.TXT`, `sfs_readback` recovers it byte-for-byte via the kernel's read
algorithm). Both tools build `-Wall -Wextra`-clean. Kernel boot-and-read proof =
DDR-768.
**Layer:** host tooling + FS (M2 storage completeness). Follows DDR-765/766 (NVMe).

## Problem

Today an SFS volume is only ever created by the kernel formatting a blank disk
in place at boot (`sfs_format`, `kernel/fs/sfs/sfs.c`), and the destructive SFS
self-tests reformat it every boot — so nothing an SFS volume holds survives a
reboot, and the build cannot ship a pre-populated SFS image. M2 calls for a host
`mkfs.sfs` that writes a valid SFS image at build time, optionally provisioning
files, so a filesystem authored on the host is read back by the kernel — the
cross-reboot-persistence proof.

## On-disk format (studied from `sfs.c:sfs_format` + the read path)

- 4 KiB blocks; little-endian (host x86_64 matches the kernel target).
- Block 0 superblock, 1 root B+tree leaf, 2 root inode, 3 journal; data/metadata
  allocate from block 4 up (`next_free_block`).
- A file's data lives in its inode's `inline_extents[]` (≤4; read at `sfs_read`).
- Directory entries are **DIR-keyed B+tree slots**, key `(parent_inode<<32) |
  FNV1a32(name)`, value `sfs_dirent {inode_num, name_len, name[]}`. Inodes are
  **INODE-keyed slots**, key `SFS_KEY_INODE | ino`, value `{inode_block}`.
- The leaf lookup is a **linear scan** (`bt_search_root`), so slot order within a
  single leaf does not matter for reads (we sort by key anyway for kernel
  `bt_insert` compatibility on later writes). One leaf holds `SFS_LEAF_MAX = 14`
  slots.

**The tool `#include`s `kernel/fs/sfs/sfs.h`** (host-includable — only `<stdint.h>`
+ a `struct blk_device` forward decl) so `struct sfs_superblock/node/inode/dirent`
and `sfs_name_hash32` are the *same* definitions the kernel uses — byte-exact by
construction, no drift.

## Decision — `tools/mkfs_sfs/mkfs_sfs.c` (host C)

`mkfs.sfs <image> [--blocks N] [--file NAME=hostpath] ...`

1. Format blocks 0–3 exactly as `sfs_format` (same superblock fields:
   magic/version 2/block_size 4096/total_blocks/root_btree 1/generation 1/
   next_free_block 4/next_inode 2/txn_log_start 3/txn_log_blocks 1).
2. Root leaf (block 1) starts with the root-inode slot (`SFS_KEY_INODE|1 →
   block 2`), root inode (block 2) is `SFS_I_DIR`.
3. For each `--file NAME=hostpath` (root-level, ≤ 4×4 KiB = 16 KiB this slice):
   allocate an inode block + data blocks from `next_free_block`, write the data,
   write the inode (`size`, `extent_count`, `inline_extents[0] = {block_start,
   block_count, logical_len, comp_len 0, flags 0}`), and append two leaf slots —
   `SFS_KEY_INODE|ino → inode_block` and `(1<<32)|FNV1a32(NAME) → dirent`. Bump
   `nkeys`, `next_free_block`, `next_inode`. Sort the leaf by key.
4. Provisioned files fit in one leaf (root-inode slot + 2 per file → ≤ 6 files);
   error out (not silently truncate) if that or the 16 KiB/file cap is exceeded.

## Gate — `smoke-mkfs-sfs` (new; host, no QEMU)

Makefile builds `build/mkfs.sfs` and `build/sfs_readback` (both host `cc`,
`-Ikernel/fs/sfs`), writes `build/mkfs_sfs.img` provisioning `/PERSIST.TXT` with
a known marker, then `sfs_readback` recovers it. `sfs_readback` uses the kernel's
`sfs.h` structs and mirrors the kernel read path exactly (linear leaf scan for
the DIR key → INODE slot → inode block → inline extents), so a byte match proves
mkfs output is decodable by the kernel reader. Runs in ~1 s (no boot).

The **kernel-boots-and-reads** end-to-end proof (attach the image as a disk the
kernel does not reformat, `vfs_mount` it, `vfs_open`/`vfs_read` `/PERSIST.TXT` →
`PRADYOS_SFS_PERSIST_OK`) is **DDR-768** — small and well-scoped now that the
mount/read pattern (`vfs_mount(idx)` + `vfs_open`) and byte-layout are confirmed.

## Non-goals (this slice)

- The kernel boot-and-read proof (DDR-768, above).
- Nested directories (root-level files only); migrating `/etc/aether/config`
  provisioning off the kernel (DDR-760/761) onto mkfs is a later follow-on once
  nested dirs land.
- LZ4-compressed extents, snapshots, journal entries (empty journal only).
- Writing through the free-extent tree (high-water `next_free_block` only, like a
  fresh format).
