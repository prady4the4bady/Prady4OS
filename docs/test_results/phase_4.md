# Phase 4 — Filesystem — Test Results

## Slice 4a: VFS switch + read-only FAT32 (capability-gated)

- **Date:** 2026-06-18
- **Decision:** ADR-015 (amends ADR-014 for multi-instance/serialized virtio-blk).
- **Files:** `kernel/fs/vfs/{vfs,vfs}.{c,h}`, `kernel/fs/fat32/{fat32}.{c,h}`,
  `kernel/cap.h` (CAP_FS_READ/WRITE/ADMIN), `kernel/drivers/blk/virtio_blk.c`
  (multi-instance + per-device serialization), `kernel/drivers/virtio/virtio_pci.{c,h}`
  (per-device BAR mapping windows), `kernel/main.c` (fs/blk self-test threads),
  `Makefile` (FAT image target, fs include dirs), `tools/qemu_runner/boot_test.sh`
  (second virtio-blk data disk).

### Commands

```bash
make image && make smoke    # kernel gate: boots q35, greps the kernel sentinel
make smoke-fs               # FS gate: builds the FAT32 disk, asserts BOTH the
                            # kernel sentinel AND the FAT32 read self-test line
```

`make smoke` does not depend on the FAT image — the kernel gate must not fail
when the host lacks `mtools`/`dosfstools`. `make smoke-fs` builds a 64 MiB
`mkfs.fat -F 32` volume with `/HELLO.TXT` and runs the boot test with
`EXTRA_SENTINEL='PRADYOS filesystem works!'`. CI runs both (it now installs
`dosfstools` + `mtools`).

### Verified (QEMU q35, two virtio-blk disks)

```
virtio-blk: blk0 ready, 2048 sectors, IRQ 11
virtio-blk: blk1 ready, 131072 sectors, IRQ 11
[fs] mounted fat32 on blk1
  /HELLO.TXT  25 bytes
[fs] /HELLO.TXT: "PRADYOS filesystem works!"
[blk] read sector 0, boot sig=0x000000000000AA55  (MBR OK)
[blk] write/read round-trip OK
```

- `vfs_mount` probes each registered FS driver against each disk; FAT32 claims
  blk1 (the `mkfs.fat` volume) and rejects blk0 (the boot disk's MBR).
- The FS test thread carries a **capability** bound to `RES_FILE`/`FS_RES_ID`
  with `CAP_FS_READ`; `vfs_readdir`/`vfs_open`/`vfs_read` each `cap_authorize`
  before touching the FS — no ambient FS authority.
- `readdir` lists the root directory; `open` resolves `/HELLO.TXT` by 8.3 key;
  `read` returns the file's exact bytes (`"PRADYOS filesystem works!"`, 25 B).
- The blk0 self-test runs **concurrently** with the FS probe of blk0 without
  corruption (see debugging note).
- smoke PASS; zero warnings (`-Werror`, C + NASM).

### Debugging note (root-caused)

Two threads targeted the same disk at once — the FS mount probe reads blk0's
sector 0 to check for FAT32, while the block self-test reads/writes blk0. The
virtio-blk driver kept a single `waiter`/`done` slot per device (the ADR-014
"one in-flight request" assumption), so the second submitter overwrote the
first's wakeup state and the first thread blocked forever. Root cause was
concurrent access to one device's single request slot, not the I/O path itself.

Fixed by serializing per device: `submit()` now spins `sti; yield; cli` on a
`busy` flag until the disk is free, then holds it for the one in-flight request.
This was found only after a prior fix to **per-device BAR mapping** — the
transport had used fixed per-BAR virtual addresses, so attaching the second
disk's BARs overwrote the first disk's MMIO window (both pointed at blk1's
registers). Each mapped BAR now gets a distinct VA window from a monotonic
allocator.

### Not done yet

- FAT32 writes; long filenames (VFAT LFN); subdirectory traversal; FSInfo.
- Mount table (multiple volumes / `blkN:/` paths) — single mount for now.
- Per-mount context (FAT32 uses file-scope globals → one volume at a time).
- SOVEREIGN FS (SFS: B+ tree, versioned, atomic tx) and ext4 — later slices.
- Per-device blocked-waiter queue (serialization is a yield-spin for now).
