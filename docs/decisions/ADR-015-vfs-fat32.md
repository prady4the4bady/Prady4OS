# ADR-015: VFS switch + read-only FAT32 + per-device block serialization

- **Status:** Accepted 2026-06-18
- **Phase:** 4 (slice 4a)

## Context

Phase 4 brings up the first real filesystem on top of the Phase 3 block layer.
The goal of this slice is the smallest end-to-end path that proves the stack:
mount a real on-disk filesystem from a block device, list its root directory,
and read a file's bytes — all **capability-gated** through the NCS, per the
sovereign-security mandate (no ambient FS authority).

FAT32 (read-only) is the starter filesystem: it is simple, ubiquitous, trivially
created from the host (`mkfs.fat`), and is the on-disk format an EFI System
Partition uses — so the same reader is reusable when the UEFI path arrives. The
sovereign filesystem (SFS, B+ tree, versioned) is later work; FAT32 first keeps
this slice honest and verifiable.

## Decision

- **VFS switch** (`kernel/fs/vfs/`): filesystem drivers register a
  `vfs_fs_ops` (name, mount, open, read, readdir). `vfs_mount(blk_index)` probes
  each registered driver's `mount()` against the block device until one claims
  it. The mounted FS is held in a single global slot (one mount for now).
- **Capability-gated access**: `vfs_open`/`vfs_read`/`vfs_readdir` each take a
  `cap_t` and call `cap_authorize(current->caps, cap, RES_FILE, FS_RES_ID,
  CAP_FS_READ)` before dispatching. There is no path to the FS that bypasses the
  capability check. New rights in `kernel/cap.h`: `CAP_FS_READ`, `CAP_FS_WRITE`,
  `CAP_FS_ADMIN`.
- **FAT32 reader** (`kernel/fs/fat32/`): parse the BPB (sector 0; require
  `0xAA55`, 512 B/sector, non-zero `fatsz32`), compute FAT start and the
  cluster-2 data start, walk the FAT cluster chain (28-bit entries), 8.3 short
  names only, root directory only. `open` matches a space-padded upper-cased 8.3
  key; `read` walks the chain copying the requested byte range; `readdir`
  enumerates root entries (skips deleted/long-name/volume-label). Two PMM-page
  scratch buffers (one data/dir sector, one FAT sector); single-threaded FS
  access for now.
- **Per-device block serialization (amends ADR-014)**: virtio-blk is now
  **multi-instance** — each disk gets its own transport handle, virtqueue, and
  request buffer (`g_inst[]`), and the shared INTx handler reaps every instance.
  Because two threads can now target the same disk (the FS probe and the block
  self-test both touch blk0), `submit()` serializes per device with a `busy`
  flag and a yield-wait: a caller spins `sti; yield; cli` until the disk is free,
  then holds it for the single in-flight request. This preserves ADR-014's
  "one request in flight per queue" invariant under concurrency instead of
  letting a second caller clobber the first's `waiter`/`done` slot.

## Consequences / deferred

- **Read-only, 8.3, root-dir-only.** Writes, long filenames (VFAT LFN),
  subdirectory traversal, and FSInfo are not implemented yet.
- **Single mount, single FS instance.** A mount table (multiple volumes, paths
  like `blkN:/...`) comes with more filesystems.
- **Per-FS global state.** FAT32 keeps its geometry in file-scope globals, so
  exactly one FAT32 volume can be mounted at a time. A per-mount context struct
  is the obvious next refactor when SFS/ext4 land alongside it.
- **`read` rescans from cluster 0** for non-zero offsets (O(file) per call). A
  cluster-index cache is a later optimization; correct and fine for small files.
- **Block serialization is a yield-spin**, not a wait queue. With short, rare FS
  I/O this is adequate; a proper per-device blocked-waiter queue is the upgrade
  when many threads contend for one disk.

## Verification

QEMU q35 with a second virtio-blk disk (`build/fat.img`, a 64 MiB `mkfs.fat -F
32` volume containing `/HELLO.TXT`):

```
virtio-blk: blk0 ready, 2048 sectors, IRQ 11
virtio-blk: blk1 ready, 131072 sectors, IRQ 11
[fs] mounted fat32 on blk1
  /HELLO.TXT  25 bytes
[fs] /HELLO.TXT: "PRADYOS filesystem works!"
[blk] read sector 0, boot sig=0x...AA55  (MBR OK)
[blk] write/read round-trip OK
```

The FS test runs from a thread holding a capability bound to `RES_FILE`/
`FS_RES_ID` with `CAP_FS_READ`; mount probes both disks and claims the FAT32 one
(blk1); readdir lists the root; the file's bytes read back exactly. The block
self-test on blk0 runs concurrently without corrupting the FS thread's I/O
(serialization fix). smoke PASS; warning-free `-Werror` build (C + NASM).
