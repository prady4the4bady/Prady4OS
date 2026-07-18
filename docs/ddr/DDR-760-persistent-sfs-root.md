# DDR-760 — persistent SFS root (SFS-as-root half 2/2)

**Status:** proposed (pre-code)
**Layer:** boot/fs + user probe. M2 storage completeness 1/N.

## Problem

DDR-739 gave a process a chosen `root_mnt` (half 1/2), proven with an *ext4*-rooted
probe — because SFS could not be a durable root. Reason (SESSION_HANDOFF blocker):
the boot SFS self-tests **unmount** blk2 (`vfs_unmount(smnt)`, main.c) and then run
the destructive journal/snapshot/LZ4 tests directly on the raw block device,
leaving the SFS volume corrupted and unmounted. So no process could root at SFS
and read files at runtime.

## Decision

Chosen approach (operator-endorsed): **the low-risk single-disk sequential path**,
not a host `mkfs.sfs` and not a second disk. All user ELFs are already loaded into
memory *before* the destructive tests (they run last, on the raw device after the
unmount), so nothing depends on the SFS volume after that point. Therefore, purely
**additively** — after the LZ4 test, with zero change to the fragile ELF-load
sequence:

1. `sfs_format(sbd)` — reformat blk2 clean (the destructive tests left it dirty).
2. `int root_smnt = vfs_mount(2)` — remount the now-clean volume (new mount id).
3. Provision a persistent tree: `vfs_create(cap, root_smnt, "/etc/aether/config",
   …)` + write the AETHER config text (mkdir-p via DDR-738 walk).
4. Spawn a probe rooted there (DDR-739 hand-rolled pattern): `elf_load` the
   embedded `sfsroottest` bytes (blocked), set `tcb->root_mnt = root_smnt`,
   `sched_unblock`.

The probe reads `/etc/aether/config` from its **SFS** root and verifies the
content → `PRADYOS_SFSROOT_OK`. This proves a process can durably root at a clean,
provisioned SFS volume and read a real config path — the DDR-739 companion done
for SFS, and the substrate for the AETHER-config migration (a follow-on M2 slice
re-points the live daemon from `/AETHER.CFG` on FAT to `/etc/aether/config` on
SFS).

## Gate — `smoke-sfsroot` (new; 95 → 96)

`EXTRA_SENTINEL=PRADYOS_SFSROOT_OK`, `FORBIDDEN_SENTINEL=SFSROOT FAIL`, via
`boot_test.sh` (the SFS disk is present in every gate). The probe's success proves
reformat→remount→provision→root→read end-to-end. The existing destructive SFS
gates (`smoke-fs-sfs-rw`, `smoke-blkmq`) are unchanged — the reformat happens
after them.

## Non-goals

- No host `mkfs.sfs` (deferred; the kernel provisions the tree in-boot). No
  cross-reboot persistence (QEMU disks are ephemeral per gate; "persistent" here
  means durable within a boot, surviving the destructive self-tests).
- No live-daemon migration yet — the daemon still reads `/AETHER.CFG` on FAT
  (DDR-732); re-pointing it is the next M2 slice.
- No second SFS disk / boot disk-topology change.
- No SFS free-space GC (separate M2 item; the reformat sidesteps the leak here).
