# DDR-741 — SFS unlink + rmdir via dir-entry tombstones

**Status:** proposed (pre-code)
**Layer:** fs (SFS). Completes the file/directory lifecycle opened by DDR-738.

## Problem

SFS can create files and (DDR-738) directories, but `sfs_unlink` is a stub
(`return -1`) — nothing can be removed. The lifecycle is one-way. The B+tree has
only insert/search (no `bt_delete`), and a full CoW B+tree delete with node
merge/rebalance is large and risky.

## Decision — tombstone the directory entry (no tree delete)

`bt_insert` already **replaces** an entry with an equal key (leaf build:
"replace existing key"). So removal overwrites the name→inode DIR entry with a
**tombstone** — `inode_num == 0` (never a valid inode: the root is 1 and
`next_inode` hands out ≥2). No `bt_delete` is needed. Four small edits make the
tombstone invisible everywhere a live entry is expected:

- **`sfs_do_lookup`**: a matching key whose `inode_num == 0` reads as
  not-found.
- **`sfs_dir_walk`**: skip `inode_num == 0` entries — so `readdir` never lists a
  removed name and the empty-directory check ignores tombstones.
- **`sfs_do_create`**: a key that resolves to a tombstone is *available* —
  create proceeds (bt_insert replaces the tombstone with the new file), so a
  removed name is re-creatable.
- **`sfs_unlink`** (implemented): walk to `(parent, name)`; look it up
  (must exist, non-tombstone). If it is a **directory**, require it be empty —
  no live child of that inode (via `sfs_dir_walk`) — else `-1` (ENOTEMPTY).
  Then replace the DIR entry with the tombstone and `sfs_commit`. Handles both
  files and empty directories through the single existing `vfs_unlink` op, so
  the VFS vtable is unchanged (FAT/ext4 untouched).

Journaling/CoW are unaffected — the tombstone insert commits exactly like any
other DIR write, and snapshots keep their own (pre-tombstone) roots.

## Gate — `smoke-sfs-unlink` (83 gates)

Kernel self-test (kmain, after the DDR-738 hier-dirs test), all via the VFS
layer on the SFS disk:
1. create `/a.txt`, write+read; `vfs_unlink("/a.txt")` -> `vfs_open` now fails
   and `readdir("/")` no longer lists it.
2. re-create `/a.txt` (tombstone slot reused) -> opens again.
3. `mkdir -p /d/e` (create `/d/e/f`), then `vfs_unlink("/d")` fails (not empty);
   `vfs_unlink("/d/e/f")`, `vfs_unlink("/d/e")`, `vfs_unlink("/d")` all succeed
   in leaf-first order; `readdir("/")` no longer lists `d`.
4. `vfs_unlink("/nope")` fails (absent).
Prints `[sfs] unlink/rmdir OK`; forbidden `unlink/rmdir FAIL`. Reuses the
`smoke-fs-sfs-rw` SFS disk.

Regression: `smoke-fs-sfs-rw`, `smoke-sfs-dirs` (create/lookup/readdir on live
entries unchanged), `smoke-user` (SFS ELF loads at root), then the full suite.

## Non-goals

- **No block reclamation** — the tombstoned inode's inode block and data extents
  are not returned to the allocator yet (bounded leak until reformat; a CoW
  free-space-GC slice later). Correctness of the namespace (name gone,
  re-creatable, readdir clean) does not depend on it.
- No `bt_delete` (tree stays insert/search + tombstone); no hard links,
  no `.`/`..`.
