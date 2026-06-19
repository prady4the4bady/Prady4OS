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

## Slice 4d: SFS engine — format + mount + empty root (slice 1)

- **Date:** 2026-06-18
- **Decision:** ADR-018 (SFS engine design + phased bring-up).
- **Files:** `kernel/fs/sfs/{sfs.c,sfs.h}` (format/mount/readdir + on-disk
  superblock w/ `next_free_block`), `kernel/main.c` (SFS self-test + SFS caps),
  `Makefile` (`sfs-image` blank disk; FS gates depend on it),
  `tools/qemu_runner/boot_test.sh` (3rd virtio-blk disk).

### Verified (QEMU q35, blank 3rd virtio-blk disk)

```
virtio-blk: blk2 ready, 32768 sectors, IRQ 11
[sfs] mounted sfs on blk2; root empty — format+mount OK
```

- The kernel `sfs_format`s the blank disk in place (superblock at block 0 +
  empty root B+ tree leaf at block 1, 4 KiB blocks = 8 sectors each).
- `vfs_mount(2)` probes the registered drivers: FAT32 declines (no `0xAA55`/BPB),
  SFS claims it by superblock magic — exercising the multi-FS mount table
  (ADR-017) with FAT32 (blk1) and SFS (blk2) mounted simultaneously.
- The empty root mounts and lists no entries (B+ tree leaf, 0 keys).
- `make smoke-fs` asserts the `format+mount OK` line alongside the FAT32
  patterns; `-Werror` clean.

### Not done yet (after 4d)

- SFS B+ tree insert/lookup (create/open), file extents (read/write), journal +
  atomic commit, snapshots, inline LZ4, free-space B+ tree — ADR-018 slices 2-6.
- FAT32 LFN/timestamps; ext4; VFS mount-point namespace.

## Slice 4e: SFS CoW B+tree — create / lookup / open / readdir

- **Date:** 2026-06-18
- **Decision:** ADR-018 (on-disk format BINDING section + phased bring-up #2).
- **Files:** `kernel/fs/sfs/{sfs.c,sfs.h}` (CoW B+tree, inode records, dir/inode
  keys), `kernel/main.c` (10-file create/lookup self-test), `Makefile`
  (smoke-fs SFS assertion).

### Verified (QEMU q35, SFS volume on the 3rd disk)

```
[sfs] mounted sfs on blk2
[sfs] created 10, verified 10, dir entries 10 - create/lookup OK
```

- `sfs_create` allocates an inode block + inode number, inserts an INODE entry
  (`inode_number → inode_block`) and a DIR entry (`(parent<<32)|hash → inode`).
- Copy-on-write insert: a leaf/internal overflow splits into freshly allocated
  blocks and the path to the root is rebuilt; nothing live is mutated; the new
  root is published by the superblock write (the commit point).
- 10 files = 21 keys > 14/leaf, so the root leaf splits and a 2-level tree
  forms — exercising split, multi-level descent, and root growth.
- `readdir` enumerates by **in-order tree walk** (the `next_leaf` chain is left
  stale by CoW splits, so it is not used for enumeration).
- `lookup` re-checks the stored name to guard FNV-1a hash collisions.
- `_Static_assert`s pin every on-disk struct to its exact size (caught a
  superblock padding miscalculation at compile time).
- `make smoke-fs` asserts the `create/lookup OK` line. `-Werror` clean.

### Not done yet (after 4e)

- SFS file extents (read/write) — slice 4f; journal (4g); snapshots (4h);
  LZ4 + tags (4i); free-space B+tree.
- Full internal-node split is implemented but only exercised at large-dir scale.

## Slice 4f: SFS file extents — read / write

- **Date:** 2026-06-18
- **Decision:** ADR-018 (phased bring-up #3).
- **Files:** `kernel/fs/sfs/sfs.c` (`sfs_read`/`sfs_write`, `inode_block_of`),
  `kernel/main.c` (64 KiB extent self-test), `Makefile`
  (`smoke-fs-sfs-rw` gate + SFS assertions in `smoke-fs`),
  `.github/workflows/ci.yml` (SFS rw gate).

### Verified (QEMU q35, SFS volume)

```
[sfs] created 10, verified 10, dir entries 10 - create/lookup OK
[sfs] 64K write/read byte-exact OK, grow to 69632 OK
```

- `sfs_write` (append/grow, `off == size`): allocates a contiguous extent from
  the high-water allocator, writes the data blocks, records an inline extent,
  CoWs the inode to a new block, and repoints the INODE B+tree entry; commit via
  the superblock write.
- `sfs_read` walks the inode's inline extents in order, copying `[off,off+len)`
  clamped to file size.
- Self-test: write 64 KiB (one 16-block extent), read it back and `memcmp`
  byte-exact, then write 4 KiB past EOF and confirm the inode size is 69632.
- New gate `make smoke-fs-sfs-rw` asserts the SFS create/lookup + byte-exact +
  grow lines; CI runs it. `-Werror` clean.

### Not done yet (after 4f)

- SFS journal + atomic commit (4g); snapshots (4h); inline LZ4 + tags (4i).
- Mid-file overwrite; >4 extents (EXTENT-keyed spill); free-space B+tree.

## Slice 4g: SFS journal + atomic transactions

- **Date:** 2026-06-18
- **Decision:** ADR-018 (phased bring-up #4).
- **Files:** `kernel/fs/sfs/{sfs.c,sfs.h}` (journal record, CRC32, txn_begin/
  commit/abort, mount replay, `sfs_selftest_journal`), `kernel/fs/vfs/{vfs.c,
  vfs.h}` (txn + umount ops, `vfs_unmount`/`vfs_txn_*`), `kernel/fs/fat32/
  fat32.c` (umount), `kernel/main.c`, `Makefile`, `.github/workflows/ci.yml`.

### Verified (QEMU q35, SFS volume)

```
[sfs] journal abort/commit/replay OK
```

`sfs_selftest_journal` runs three crash scenarios end-to-end with real
mount/unmount cycles on the SFS disk:

1. **abort discards** — `txn_begin`, create AAA, `txn_abort`, remount → AAA
   absent (uncommitted CoW blocks forgotten; the superblock was never written).
2. **commit persists** — `txn_begin`, create BBB, `txn_commit`, remount → BBB
   present (journal record + superblock both written).
3. **torn-commit replay** — `txn_begin`, create CCC, write the journal record
   but NOT the superblock (a crash between the two writes), remount → recovery
   sees `journal.txn_id > superblock.generation`, CRC-validates, and replays it
   → CCC present.

- Transactions defer the superblock write so a batch publishes atomically; abort
  rolls back the in-memory root + `next_free`.
- Design: SFS is copy-on-write, so the journal is a *logical* commit record (the
  root swap), not a physical shadow-block log — the correct CoW equivalent.
- `make smoke-fs-sfs-rw` (and `smoke-fs`) assert the journal line; CI runs it.
  `-Werror` clean.

### Not done yet (after 4g)

- SFS snapshots (4h); inline LZ4 + 4 KiB tags (4i); free-space B+tree.
- ext4 read + FAT32 LFN/timestamps (4j).

## Slice 4h: SFS snapshots (version isolation)

- **Date:** 2026-06-18
- **Decision:** ADR-018 (phased bring-up #5).
- **Files:** `kernel/fs/sfs/{sfs.c,sfs.h}` (superblock snapshot table,
  `sfs_snapshot`/`sfs_open_version`, root-parameterised `bt_search_root`/
  `inode_block_of_root`, versioned-read via `vfs_file.dirent_clus`,
  `sfs_selftest_snapshot`), `kernel/main.c`, `Makefile`.

### Verified (QEMU q35, SFS volume)

```
[sfs] snapshot version-isolation OK
```

- A snapshot is a retained B+ tree root captured at a generation (16-entry table
  in the superblock). CoW + the non-reclaiming high-water allocator keep the
  captured root and all reachable blocks valid with no extra work.
- `sfs_selftest_snapshot`: write 4 KiB pattern A to VER (v1), snapshot, append
  4 KiB pattern B (v2 → size 8192), then `open_version(snapshot)` and read →
  exactly 4 KiB of pattern A (v1 intact), while the current handle shows size
  8192. A versioned handle reads through the snapshot root and refuses writes.

### Not done yet (after 4h)

- Free-space B+ tree + snapshot GC (high-water allocator is non-reclaiming;
  dropped-snapshot blocks not yet returned) — own sub-slice, persisted-format.
- Inline LZ4 + 4 KiB metadata tags (4i); ext4 read + FAT32 LFN/timestamps (4j).

## Slice 4i: SFS inline LZ4 + metadata tags

- **Date:** 2026-06-18
- **Decision:** ADR-018 (phased bring-up #6).
- **Files:** `kernel/fs/sfs/lz4.{c,h}` (LZ4 block-format codec, bounds-checked
  decompressor), `kernel/fs/sfs/{sfs.c,sfs.h}` (per-extent compression, extent
  ref carries logical_len/comp_len/flags, `sfs_set_tag`/`sfs_get_tag`,
  `sfs_selftest_lz4`), `kernel/main.c`, `Makefile` (lz4.o + gate assertion),
  `.github/workflows/ci.yml`.

### Verified (QEMU q35, SFS volume)

```
[sfs] lz4+tags compress/readback/tag OK
```

- **Per-extent compression** (design correction): each write becomes one extent,
  compressed independently if it saves >25% (`SFS_EXT_LZ4`), so a compressed file
  can still be appended. (An earlier whole-file approach broke append for the 4f
  grow / 4h snapshot tests — their data is compressible — and was replaced.)
- `sfs_selftest_lz4`: write a 128 KiB highly-compressible file → lands in <32
  blocks with the LZ4 flag, reads back byte-exact (decompress), and a metadata
  tag round-trips across a remount.
- LZ4 decompressor is fully bounds-checked (malformed input returns 0, never
  overruns) per the security rules.

### Not done yet (after 4i)

- ext4 read-only + FAT32 LFN/timestamps (slice 4j) → then the Layer 4 gate.
- Free-space B+ tree + snapshot GC; mid-file overwrite; >4-extent EXTENT spill.

## Slice 4j: ext4 read-only + FAT32 long names + RTC timestamps

- **Date:** 2026-06-18 / 2026-06-19
- **Decision:** ADR-019 (ext4), ADR-020 (RTC + FAT32 LFN/timestamps).
- **Files:** `kernel/fs/ext4/ext4.{c,h}`, `kernel/drivers/rtc/rtc.{c,h}`,
  `kernel/fs/fat32/fat32.c` (LFN reconstruction + name matching + timestamps),
  `kernel/main.c`, `Makefile` (ext4-image, smoke-fs-ext4, LFN/RTC assertions),
  `.github/workflows/ci.yml` (e2fsprogs + ext4 gate).

### Verified (QEMU q35, four filesystem disks)

```
[ext4] mounted ext4; /EXT4.TXT: "ext4 read works"
[fs] /LongFileName.txt: "long name read works"
[rtc] 2026-6-19 14:49
```

- **ext4 read-only** (part 1): superblock + group descriptors + extent-mapped
  inodes (depth-0) + linear directory scan; reads a host `mkfs.ext4 -d` volume.
- **FAT32 VFAT long names** (part 2): `dir_scan` reconstructs long names and
  matches path components case-insensitively (long or 8.3); all lookups switched
  to name-matching. 8.3 + nested reads unregressed.
- **RTC + timestamps**: CMOS clock read; FAT32 create/write stamp real dates.

### Layer 4 completion gate — ALL PASS

```
smoke           PASS   (kernel boot)
smoke-fs        PASS   (FAT32 RO/LFN/RTC + SFS create/lookup/extents/journal/
                        snapshot/LZ4 assertions)
smoke-fs-rw     PASS   (FAT32 write + host fsck.fat/mdir/mtype)
smoke-fs-sfs-rw PASS   (SFS CoW B+tree + extents + journal + snapshot + LZ4)
smoke-fs-ext4   PASS   (ext4 read-only)
```

CI runs all five on `main`. Layer 4 (VFS + FAT32 RW + full SFS engine + ext4 RO)
is complete; `-Werror` clean throughout.

### Deferred (tracked, post-Layer-4)

- FAT32 LFN *write*; full UTF-8; SFS free-space B+ tree + snapshot GC; SFS
  mid-file overwrite + >4-extent EXTENT-keyed spill; ext4 write + multi-level
  extents + block-mapped inodes; VFS mount-point namespace.
