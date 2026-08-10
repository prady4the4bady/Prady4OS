= DDR-889 — on-disk SFS free list (Group 5 item 31)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `kernel/fs/sfs/sfs.{c,h}`, `kernel/main.c`, `smoke-fs`.

## What was wrong

`c->free_runs[]` was in-memory only (DDR-762-v2). Every reclaimed run was
discarded at unmount, so a volume leaked blocks across a remount: `next_free`
climbed forever even on a filesystem that was mostly free space. The superblock
already reserved `free_extent_tree` for exactly this and it was always 0.

## The on-disk shape is a list, not a tree

One block: `{magic, count, runs[254]}`. Despite the field name, **not** a B+tree.

The in-memory model is a bounded 256-entry array with exact-fit and no
coalescing. A tree would be a second structure with different semantics to keep
in step with it, and the reconciliation is where the bugs would live. 254 runs ×
16 bytes fits one block by construction — the same bound, persisted.

The block is allocated **once** and reused in place. Allocating a fresh one per
sync would consume a block per commit: a leak dressed as an allocation.

## What is rejected rather than absorbed

- A block whose magic does not match → the list is **dropped**. Loading whatever
  bytes happen to be there would hand the allocator arbitrary "free" runs, and
  the next write would land on live blocks — silent cross-file corruption.
  Dropping it leaks, which is bounded and safe.
- A run extending past `next_free` → skipped. It is not a block this filesystem
  ever allocated, so it cannot be free.
- The save happens **before** the superblock write, because the superblock is the
  commit point: a root recorded for a block not yet written would survive a crash
  pointing at stale bytes.

## The first gate passed for the wrong reason, and was deleted

The obvious test was written first: unmount, remount, run a create/write/unlink
cycle, require `next_free` not to grow. It passed.

It was **worthless**. With the on-disk load disabled — the exact pre-item-31
behaviour — it *still* passed: `grew=10` against `grew=9` with loading enabled.
`vfs_mount()` returns the **cached** mount, so no fresh context is built and the
in-memory runs survive; the measurement was the same either way.

It was removed rather than retuned. A threshold placed between 9 and 10 would
have "discriminated" while testing nothing about persistence.

## What ships instead: a smaller claim that is true

`sfs_read_freelist_count()` reads the superblock **off the device**, follows
`free_extent_tree`, checks the magic, and returns the run count. The self-test
requires it to be present and non-empty:

```
[sfs] freelist ondisk runs=1
[sfs] freelist persist OK
```

| Mutation | Applied? | Result |
|---|---|---|
| never call `sfs_freelist_save` | verified yes | **killed** |
| write the wrong magic to the block | verified yes | **killed** |

Both mutations were confirmed present in the source before the verdict was
trusted — the third time in this project that an unapplied `sed` read exactly
like a surviving mutant.

## What is proven, and what is not

**Proven:** the free list reaches the device, carries its magic, holds runs, and
the superblock names it. Removing either the save or the identity check fails
the gate.

**Not proven: reuse after a genuine cold remount.** Every mount in a single boot
is served from cache, so nothing here re-reads the list into a fresh context. The
load path is exercised only at first mount, where the list is empty. Proving the
round trip needs either a second boot against a persisted image (the
`smoke-sfs-persist` mkfs-image pattern) or a `vfs_unmount` that genuinely
destroys the context — and that is a VFS change, not an SFS one.

Stated rather than implied, because "the free list persists" and "a freed block
is reused after reboot" are different claims and only the first is gated.

**Group 5 item 31 complete for the persistence half; the cold-remount reuse
round trip is named as the follow-up.**
