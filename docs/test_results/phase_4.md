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

### Not done yet (after 4a)

- FAT32 writes; long filenames (VFAT LFN); FSInfo.
- Mount table (multiple volumes / `blkN:/` paths) — single mount for now.
- Per-mount context (FAT32 uses file-scope globals → one volume at a time).
- SOVEREIGN FS (SFS: B+ tree, versioned, atomic tx) and ext4 — later slices.
- Per-device blocked-waiter queue (serialization is a yield-spin for now).

## Slice 4b: FAT32 subdirectory traversal (nested paths)

- **Date:** 2026-06-18
- **Decision:** ADR-015 (extended).
- **Files:** `kernel/fs/fat32/fat32.c` (component-based path resolver:
  `comp_key`, `dir_scan`, `walk_dir`; rewrote `open`/`readdir`),
  `kernel/fs/vfs/{vfs.c,vfs.h}` (`readdir` gains a `path` arg), `kernel/main.c`
  (fs self-test reads a nested file + lists a subdirectory), `Makefile`
  (`/DOCS/NOTE.TXT` added to the FAT image; `smoke-fs` asserts both files),
  `tools/qemu_runner/boot_test.sh` (multi-pattern `EXTRA_SENTINEL`).

### Verified (QEMU q35, FAT32 disk with a subdirectory)

```
[fs] mounted fat32 on blk1
[fs] dir /:
    HELLO.TXT  25 bytes
    DOCS  0 bytes
[fs] /HELLO.TXT: "PRADYOS filesystem works!"
[fs] dir /DOCS:
    .  0 bytes
    ..  0 bytes
    NOTE.TXT  14 bytes
[fs] /DOCS/NOTE.TXT: "nested file ok"
```

- `open("/DOCS/NOTE.TXT")` descends component-by-component: each non-final
  component must carry `ATTR_DIRECTORY`; the final must be a regular file.
- `readdir("/DOCS", ...)` resolves the directory via `walk_dir` then enumerates
  it (including `.`/`..`); `readdir("/", ...)` lists the root as before.
- `make smoke-fs` now asserts BOTH `"PRADYOS filesystem works!"` (root) and
  `"nested file ok"` (nested) — the nested assertion fails unless path descent
  works. smoke + smoke-fs PASS; `-Werror` clean.

### Not done yet (after 4b)

- FAT32 **writes** (next slice 4c): cluster allocation, FAT + mirror update,
  directory-entry create/update, FSInfo.
- Long filenames (VFAT LFN); relative paths / cwd; per-mount context.
- SOVEREIGN FS (SFS) and ext4 — later phase-4 slices.

## Slice 4c: FAT32 write + VFS mount table + SFS skeleton

- **Date:** 2026-06-18
- **Decision:** ADR-015 (FAT32 write), ADR-016 (preemptive concurrency), ADR-017
  (VFS mount table + SFS skeleton).
- **Files:** `kernel/fs/vfs/{vfs.c,vfs.h}` (mount table + ctx ops + write/create/
  unlink), `kernel/fs/fat32/fat32.c` (per-mount ctx + write path), `kernel/fs/
  sfs/{sfs.c,sfs.h}` (skeleton), `kernel/cap.h` (CAP_FS_SFS_*), `kernel/proc/
  sched.{c,h}` (write budget + atomic schedule), `kernel/mm/pmm.c`,
  `kernel/console.c` (interrupt-atomic), `kernel/main.c`, `Makefile`,
  `tools/qemu_runner/boot_test.sh`.

### Verified (QEMU q35) — in-kernel write→read-back

```
[fs] wrote /KOUT.TXT (17 bytes)
[fs] /KOUT.TXT readback: "kernel wrote this"
[fs] created+deleted /TMP.TXT OK
```

### Verified (host-side adversarial validation, `make smoke-fs-rw`)

After the kernel creates/writes `/KOUT.TXT` and create+deletes `/TMP.TXT`, the
host inspects the same image QEMU wrote back:

```
fsck.fat -n -v build/fat.img   ->  "5 files, 5/129022 clusters"  (consistent)
mdir   -i build/fat.img ::/    ->  HELLO.TXT, DOCS, KOUT.TXT  (no TMP.TXT)
mtype  -i build/fat.img ::/KOUT.TXT  ->  "kernel wrote this"
```

- `fsck.fat` finding no errors confirms the FAT chains, directory entry, and
  FSInfo free-count the kernel wrote are spec-correct.
- Writes are all-or-nothing on allocation (rollback on a short disk) and every
  write is read-back-verified in-kernel before returning success.
- Per-thread write budget (`tcb.fs_write_budget`, 1 MiB default) enforced in
  `vfs_write`.

### Debugging note — the real root cause (root-caused)

`make smoke-fs` failed intermittently (≈9/10) with garbled serial and a block
test pattern appearing in kernel output. It was **not** the filesystem or a data
race: the block self-test wrote its scratch pattern to **boot-disk sector 100**,
and the kernel image had grown past 50 KiB so sector 100 now lies inside the
on-disk kernel (loaded from LBA 17). QEMU persists guest writes back to
`build/pradyos.img`, so each run corrupted the kernel for the *next* boot — run 1
passed (clean image), runs 2-10 loaded a corrupted kernel. In slice 4b the
kernel was < 50 KiB so the same sector-100 write landed in padding. Fixed by
moving the scratch write to LBA 1500 (past the 256 KiB / 512-sector kernel cap,
inside the 1 MiB image). `make smoke-fs` then passes repeatably.

While diagnosing, three genuine preemptive-concurrency gaps were also fixed
(ADR-016): unlocked PMM free-lists, interleaving console writes, and a
re-enterable `schedule()`.

### Not done yet (after 4c)

- FAT32 long filenames (VFAT LFN); real timestamps (needs an RTC driver).
- Per-mount FS lock + block-layer multi-request queue for fully concurrent
  multi-thread I/O (ADR-016 / build_status DEFERRED).
- SFS engine (CoW B+ tree, versioning, atomic tx, LZ4) and ext4.
- VFS mount-point namespace (`/mnt/...`).
