# DDR-SMP-3c-locks-3 — per-mount VFS serialization (ADR-030 stage 3c)

> DDR before code. Continues the full-3c prerequisite campaign: after the block
> layer (locks-2), make the VFS layer cross-CPU safe.

## The gap locks-2 does NOT close
`virtio_blk`'s atomic `busy` (locks-2) serializes the *device DMA*. But a VFS
write touches the FS driver's **in-memory metadata** — the SFS journal state,
CoW B-tree nodes, the FAT allocation cursor — *around* the block calls, and
that metadata has no lock. Two threads on two CPUs in `vfs_write` on the same
mount would interleave those mutations and corrupt the filesystem regardless of
the block serialization below them.

## Decisions
- **D1 — per-mount sleep-mutex.** `struct vfs_mount` gains `busy`; the
  data-path entry points (`open`/`create`/`read`/`write`/`unlink`/`readdir`/
  `txn_begin`/`txn_commit`/`txn_abort`) take it around the `m->fs->…` call via
  `mnt_lock`/`mnt_unlock` (`__atomic_exchange_n` acquire + yield-loop /
  `__atomic_store_n` release). It is a **sleep-mutex, NOT a spinlock**: an FS op
  descends into `blk` `submit()` which `sched_block()`s, and a spinlock held
  across a block deadlocks spinners (the locks-2 lesson). Lock order is always
  **mount → blk**, never reversed — no ABBA. No IRQ touches `busy`, so (unlike
  `virtio_blk`) no `cli/sti` is needed.
- **D2 — the mount table stays BSP-only.** `vfs_mount`/`vfs_unmount` mutate the
  `g_mounts[]` topology and are boot/teardown operations run only by the BSP
  (mirrors locks-1's "topology mutations remain BSP-only"). `mnt_get`'s read of
  `used` is safe under that invariant; a future dynamic-mount slice locks the
  table itself. `vfs_fs_name` reads the immutable `fs` pointer — no lock.
- **D3 — scope: corruption safety, not transaction isolation.** Per-op locking
  makes each op's metadata mutation atomic across CPUs; it does NOT make a
  multi-syscall `txn_begin…write…commit` sequence mutually exclusive between
  threads. That interleaving is already possible on ONE CPU (time-slicing) —
  this slice preserves exactly today's single-CPU guarantees across CPUs, no
  more. Transaction ownership is a separate, larger design (non-goal).
- **D4 — no re-entrancy.** FS drivers (`sfs`/`fat32`/`ext4`) call `blk`
  directly, never back into `vfs_*`, so an FS op cannot recursively re-take its
  own mount lock. (Verified: no `vfs_` calls under `kernel/fs/{sfs,fat32,ext4}`.)

## Gate
None new. The FS gates (`smoke-fs`/`-rw`/`-sfs-rw`/`-ext4`) + `smoke-user`
exercise every locked path each run; behavior on the one CPU that does VFS work
today is unchanged. 58 gates.

## Non-goals
Transaction mutual exclusion (D3); a reader/writer split (conservative
all-ops-exclusive is correct, an rwlock is an optimization); locking the mount
table for dynamic mount/unmount; per-open-file locks.
