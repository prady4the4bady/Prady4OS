# DDR-768 — cross-reboot SFS persistence proof (kernel reads the mkfs image)

**Status:** implemented — `smoke-sfs-persist` PASS (`PRADYOS_SFS_PERSIST_OK`: the
kernel mounts a host-authored mkfs.sfs image and reads back `/PERSIST.TXT`).
Image builds `-Werror`-clean.
**Layer:** kernel FS + gates (M2 storage completeness). Follows DDR-767 (host
`mkfs.sfs`).

## Problem

DDR-767 gave us a host `mkfs.sfs` that writes byte-exact SFS images, proven
decodable by a host verifier that mirrors the kernel read path. The remaining
proof is the real one: the **kernel itself** boots on a host-authored image it
did **not** format and reads a provisioned file back — end-to-end host→kernel SFS
interop and the cross-reboot-persistence guarantee.

## Findings (from reading the tree this session)

- Mount/read pattern: `int m = vfs_mount(blk_index)` (SFS claims a disk by block-0
  magic), then `vfs_open(cap, m, "/PATH", &f)` + `vfs_read(cap, &f, off, buf, len)`
  — exactly as the existing SFS self-test (`main.c` ~line 858).
- **`VFS_MAX_MOUNTS = 4`** (`vfs/vfs.h`) and mounts are NOT deduped — each
  `vfs_mount` consumes a fresh slot and never auto-unmounts. The boot already
  holds fat(1)+sfs(2)+ext4(3) (+ the SFS root), so there may be **no free slot**
  for a persist mount. → this slice must **bump `VFS_MAX_MOUNTS`** (4 → 6) or
  `vfs_umount` a scratch mount first. Bumping is the clean enabler.
- Registration order follows the QEMU `-device` order → the mkfs disk, attached
  **last**, lands at `blk_count() - 1`. Peek its block 0 for `SFS_MAGIC` via
  `blk_read` before mounting, so non-persist gates (no mkfs disk) never waste a
  mount slot.

## Decision

1. `VFS_MAX_MOUNTS` 4 → 6 (`vfs/vfs.h`) — headroom for the persist mount.
2. `boot_test.sh`: `QEMU_SFS2` knob attaches `build/mkfs_sfs.img` as the **last**
   virtio-blk disk (every other gate omits it). **Also `QEMU_NO_EXT4`**: the
   virtio-blk driver caps at `VBLK_MAX = 4` instances (4 hardcoded MSI-X
   handlers; vectors 50–53, with net@54 / input@55 packed right after — so
   raising the cap needs a vector remap, out of scope here). With `build/ext4.img`
   present from earlier gates, the persist disk would be the **5th** virtio-blk
   and silently dropped. `QEMU_NO_EXT4` suppresses ext4 for this gate, keeping the
   mkfs disk within the cap at blk3. (Lifting `VBLK_MAX` to 8 with a proper MSI-X
   vector reassignment is a future slice if >4 concurrent disks are ever needed.)
3. Kernel persist self-test (guarded, runs once near the SFS self-test): if the
   highest blk index's block 0 carries `SFS_MAGIC` (raw `blk_read` peek), mount
   it, `vfs_open`/`vfs_read` `/PERSIST.TXT`, compare to the known marker, and
   print `PRADYOS_SFS_PERSIST_OK` (or `..._FAIL`). Never formats/writes it.
4. Gate `smoke-sfs-persist`: build `build/mkfs_sfs.img` (mkfs.sfs provisioning
   `/PERSIST.TXT`), boot with `QEMU_SFS2=1`, assert `PRADYOS_SFS_PERSIST_OK`;
   forbid `..._FAIL`. This is the true cross-reboot persistence proof (the
   file was authored by the host tool, read by the kernel).

## Non-goals

- Nested-directory provisioning / migrating `/etc/aether/config` off kernel
  provisioning (DDR-769, once mkfs grows nested dirs).
- Writing back to the mkfs image from the kernel (read-only proof here).
- Persisting across an actual power cycle in CI (single boot reads a host-authored
  image — the persistence guarantee is the byte-format interop, not QEMU state).
