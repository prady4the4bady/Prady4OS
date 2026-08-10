= DDR-890 — PRADYOS Drive, the agent workspace mount (Group 7 item 40)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `kernel/fs/pdrive/`, `kernel/fs/vfs/vfs.{c,h}`, `kernel/main.c`,
`smoke-fs`, `smoke-fs-sfs-rw`.

## 1. It is a mount, not a special case

PRADYOS Drive implements `struct vfs_fs_ops` exactly as fat32, SFS and ext4 do.
An agent rooted there uses `open`/`create`/`read`/`write`/`unlink`/`readdir`/
`truncate` unchanged.

The alternative — a workspace threaded through the syscall layer as its own set
of calls — would have been a second file API to keep in step with the first, and
would have proven nothing about the VFS contract. Being a mount means every
existing capability check, every `vfs_*` entry point, and every gate that
exercises the VFS covers it for free.

## 2. `vfs_mount_virtual()` selects BY NAME, and that is deliberate

`vfs_mount()` probes each registered driver against a block device and takes the
first that accepts. A filesystem with no device cannot be probed that way.

The naive fix — "mount the driver that accepts a NULL device" — breaks the moment
there are two virtual filesystems: both accept NULL, and the caller silently gets
whichever registered first. `vfs_mount_virtual(name)` matches on the driver's own
`name` and **does not fall back** if that driver refuses.

Symmetrically, `pd_mount()` **refuses a non-NULL `bd`**. Without that, the probe
loop in `vfs_mount()` would offer pdrive the first disk, pdrive would accept it,
and whatever real filesystem was on that disk would never be tried — a disk that
silently mounts as an empty RAM volume.

## 3. Bounded, because an agent controls the size

This is memory the least trusted code in the system decides how much of to
consume. An unbounded RAM filesystem is a denial-of-service primitive: an agent
writing in a loop takes every free frame, and the PMM starves the *kernel*, not
the agent.

`PDRIVE_MAX_FILES` (32) and `PDRIVE_MAX_BYTES` (1 MiB) are both enforced by
**refusal**. Clipping the write or dropping it silently would report success for
data that is not there, and the agent would proceed believing its workspace holds
something it does not. A full file table is refused rather than evicting an
existing file, for the same reason: eviction loses another agent's data without
telling anyone.

The growth path charges the **delta** against the budget before allocating, so
the cap governs what the workspace *will* hold rather than what it held a moment
ago.

## 4. The gate, and the mutations it kills

The self-test mounts the drive and runs create → write → read-back → compare →
readdir → over-capacity write → unlink → confirm-gone, then prints one line:

```
[pdrive] mounted id=3 rw OK readdir OK overflow REFUSED unlink OK
[pdrive] workspace OK
```

| Mutation | Applied? | Result |
|---|---|---|
| clip an over-capacity write instead of refusing | verified yes | **killed** |
| let the virtual FS claim a real disk (`(void)bd`) | verified yes | **killed** |

Both were confirmed present in the source before the verdict was read — the
standing rule after three separate occasions where an unapplied edit read exactly
like a surviving mutant.

M2 is worth noting: it is killed not by a pdrive assertion but by the **disk**
filesystems failing, because pdrive swallowed their device. That is the correct
blast radius for that bug, and it is why `pd_mount` refuses a device rather than
merely ignoring one.

## 5. Scope

**Not implemented:** subdirectories (the namespace is flat — one workspace, not a
tree), persistence across reboot (it is RAM by design; durable agent state
belongs on SFS), per-agent isolation into separate volumes, and grow-by-truncate.

Per-agent isolation is the one worth naming: today the drive is a single shared
workspace, and separating agents needs one mount per agent plus a mount-table
larger than `VFS_MAX_MOUNTS` (6). That is a VFS capacity change, not a pdrive
change, and inventing a private namespace inside pdrive would duplicate what
mounts already express.

**Group 7 item 40 complete.**
