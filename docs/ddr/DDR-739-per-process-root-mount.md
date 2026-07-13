# DDR-739 — Per-process root mount (selectable at spawn)

**Status:** proposed (pre-code)
**Layer:** proc + fs (VFS mount ↔ process). First half of SFS-as-process-root.

## Problem

`tcb.root_mnt` is already per-process (fork inherits it; `SYS_OPEN` resolves
against it), but it is *hard-wired*: `elf_load` sets every new process's
`root_mnt = vfs_default_mnt()` (the FAT boot volume), and nothing overrides it.
So all processes share one root, and there is no way to give a process a
different root filesystem — the prerequisite mechanism for SFS-as-root (blocked
separately by the SFS disk being consumed by destructive boot self-tests,
DDR-738 follow-up). This slice adds and proves the *selection* mechanism in
isolation, using a filesystem that already persists to scheduler time.

## Decision

**Spawn-with-root.** A process may be created with a caller-chosen mount as its
root instead of `vfs_default_mnt()`. `elf_load` still returns the thread BLOCKED
with the default root; the spawner sets `t->root_mnt = <chosen>` **before**
`sched_unblock` (closing the create-then-init race, the DDR-SMP-3c-cap-2a D3
pattern). No new syscall — this is kernel-side spawn policy; fork continues to
inherit the parent's root verbatim, so an ext4-rooted process's children are
ext4-rooted too.

**Proof vehicle — ext4 (blk3).** ext4 mounts read-only at boot and is **never
unmounted** (it survives to scheduler time, unlike SFS), and holds `/EXT4.TXT`
("ext4 read works") which does **not** exist on the FAT default root. So a
ring-3 probe rooted at ext4 that (a) opens `/EXT4.TXT` successfully and (b) fails
to open `/HELLO.TXT` (which is on FAT, not ext4) demonstrates two-sided that its
resolution used the *selected* ext4 root, not the global FAT default.

**Boot wiring.** The single ext4 mount is taken once, early (kept in a kmain
local), reused by both the existing ext4 self-test (previously re-mounted at its
own site) and the probe — staying within `VFS_MAX_MOUNTS=4` (FAT + SFS + ext4).
The probe (`user/rootmounttest.c`, freestanding, musl-free) is spawned via
`elf_load` from embedded bytes (SFS is being torn down; no SFS round-trip) with
its `root_mnt` set to the ext4 mount, only when blk3 is present — so gates
without the ext4 disk simply don't spawn it (no effect).

## Gate — `smoke-rootmount` (82 gates)

Depends on `ext4-image` (attaches disk3, as `smoke-fs-ext4` does). Asserts
`PRADYOS_ROOTMOUNT_OK` — printed by the probe iff `/EXT4.TXT` opens from its
ext4 root AND `/HELLO.TXT` (FAT-only) does not. Forbidden `ROOTMOUNT FAIL`.
Regression: `smoke-fs-ext4` (the reused single ext4 mount must still read
`/EXT4.TXT`), `smoke-user`, `smoke-fs*`, then the full suite.

## Non-goals (the SFS-as-root follow-on)

- **Not** the SFS root: that additionally needs a *persistent* SFS volume (the
  destructive self-tests reformat the only SFS disk) + image-time provisioning
  of `/etc/aether/config`. This slice proves only the root-selection mechanism.
- No per-process root *syscall* (chroot); no writable ext4 (it is read-only).
- No change to `vfs_default_mnt()` — existing processes stay FAT-rooted.
