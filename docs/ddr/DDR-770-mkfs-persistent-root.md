# DDR-770 — persistent root `/etc/aether/config` from a host mkfs.sfs image

**Status:** implemented — `smoke-aether-sfsroot` PASS (`[sfs] AETHER daemon
rooted at provisioned mkfs image` + `PRADYOS_AETHER_CFG_OK mode=sovereign`: the
daemon reads its policy from a host-built image, kernel provisioning skipped).
`smoke-sfs-persist` regression green after decoupling its nested marker to
`/etc/test/config`. Follows DDR-767/768/769.

## Problem

Today the AETHER boot policy `/etc/aether/config` is provisioned **by the
kernel** at boot (DDR-760/761): `main.c` reformats blk2, mounts it, and
`vfs_create`+`vfs_write`s the config text, then roots the daemon there. Now that
mkfs.sfs can author nested paths (DDR-769) and the kernel reads host images
(DDR-768), the config should ship **in a build-time image** — no kernel
provisioning.

## Constraints found (why this is careful work)

- **blk2 is dual-use**: the destructive SFS self-tests (create/churn/journal/
  snapshot/unlink) format and dirty blk2 *before* it becomes the persistent root,
  so the provisioned root cannot be blk2 — it must be a **separate disk**.
- **`VBLK_MAX = 4`** caps virtio-blk instances (MSI-X vectors 50–53 packed vs
  net@54/input@55). The AETHER gate attaches only boot(0)/fat(1)/sfs(2), so a
  provisioned-root disk fits as **blk3** — no vector remap needed here. (Boots
  that also carry ext4 would need the deferred `VBLK_MAX` lift; not this gate.)
- The daemon already falls back to its **compiled default config** if
  `/etc/aether/config` can't be read, so rooting it at a disk without the file is
  non-fatal — the change is safe to guard.

## Decision

1. Build `build/sfsroot.img` via `mkfs.sfs --file /etc/aether/config=<cfg>` with
   the same policy text the kernel used (`mode=sovereign\ntask=test\nslot=0\n
   net=10.0.2.2:11434\n`).
2. `boot_test.sh` gains `QEMU_SFSROOT` — attach `build/sfsroot.img` as an extra
   virtio-blk disk (blk3 in the AETHER gate).
3. Kernel persistent-root block (`main.c`, DDR-760/761): **before** the
   reformat-blk2 fallback, look for a pre-provisioned SFS root — a blk index
   whose block 0 is `SFS_MAGIC` and which contains `/etc/aether/config` (peek +
   `vfs_open`, mirroring DDR-768). If found, mount it as `root_smnt` and root the
   AETHER daemon there **without** `sfs_format` or `vfs_create`/`vfs_write` of the
   config — print `[sfs] daemon rooted at provisioned image`. Else keep the exact
   current behavior (reformat blk2 + kernel-provision). Default boots (no
   provisioned disk) are unchanged.
4. New gate `smoke-aether-sfsroot`: boot with `QEMU_SFSROOT=1`, assert
   `PRADYOS_AETHER_CFG_OK mode=sovereign` (daemon read the SHIPPED config) plus
   the provisioned-root sentinel; forbid a config-read failure.

## Non-goals / follow-on

- Making the provisioned root the DEFAULT for every boot (needs the `VBLK_MAX`
  lift so it coexists with ext4, and retiring blk2's dual role) — deferred.
- Multi-key/edited configs, versioned policy — the shipped image is static.
