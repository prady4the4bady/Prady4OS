# DDR-738 — SFS hierarchical directories (path walk + dir inodes + mkdir-p)

**Status:** proposed (pre-code)
**Layer:** fs (SFS). Prerequisite for SFS-as-process-root (a later slice).

## Problem

SFS stores directory entries under keys `(parent_inode << 32) | hash32(name)`,
so the B+tree already *represents* arbitrary hierarchy — but the code doesn't
use it. `sfs_open`/`sfs_create` resolve exactly one path component under
`SFS_ROOT_INODE`; `sfs_readdir` ignores its `path` and always lists the root;
inodes carry no file-vs-directory distinction. So `/etc/aether/config` (the
DDR-732 intended config path, deferred pending "SFS as process root") is
unrepresentable. This slice adds the missing *algorithm* — hierarchical path
resolution — while leaving the process root untouched (FAT), so no user-facing
gate changes.

## Decision — SFS-local path walk, mkdir-p on create

**Directory inodes.** A new inode flag `SFS_INO_DIR` (bit in the existing
`sfs_inode.flags`) marks a directory. The root (`SFS_ROOT_INODE`) is a directory
by definition (it predates any inode block, so the walk treats inode 1 as a dir
without reading a block — back-compatible with existing volumes). `sfs_do_create`
gains an `is_dir` parameter that sets the flag.

**Path walk (`sfs_walk`).** Split the path on `/` (ignoring empties). Starting at
`SFS_ROOT_INODE`, for each component call `sfs_do_lookup(parent, comp)`:
- returns the child inode, and the parent must be a directory (checked via its
  inode flag; root is implicitly a dir).
- The walk yields `(parent_inode, final_component)` for create/open, or the
  resolved inode for a full-path lookup.

**`sfs_open`**: walk all-but-last requiring each to exist AND be a directory
(`-1` otherwise — no descending through a file); look up the last component in
its parent; open it as a file.

**`sfs_create`**: walk all-but-last with **mkdir-p** — a missing intermediate is
created as a directory inode (`sfs_do_create(..., is_dir=1)`); an existing
intermediate that is a *file* is an error. Create the final component as a file
under its parent. This provisioning model matches how the config path is laid
down (no separate `mkdir` NSI needed; the VFS vtable is unchanged, so FAT/ext4
are untouched).

**`sfs_readdir`**: walk `path` to its directory inode (root when empty) and list
that inode's children (the existing `sfs_dir_walk` already filters by `parent`
— pass the resolved inode instead of the hard-coded `SFS_ROOT_INODE`).

Hash-collision and name-length guards in `sfs_do_lookup` are unchanged and apply
per component. Journaling/CoW are unaffected — each intermediate `sfs_do_create`
commits exactly as a root-level create does today.

## Gate — `smoke-sfs-dirs` (81 gates)

A kernel self-test (kmain, after the existing SFS r/w proof) via the VFS layer:
1. `vfs_create("/etc/aether/config")` — mkdir-p creates `/etc` and `/etc/aether`,
   then the file; `vfs_write` a known payload; `vfs_read` it back byte-exact.
2. `vfs_open("/etc/aether/config")` from scratch (fresh walk) returns the same
   bytes — proving resolution, not just the just-created handle.
3. `vfs_open("/etc")` fails as a file open (it's a directory), and
   `vfs_open("/etc/nope/x")` fails (missing intermediate) — negative cases.
4. `vfs_readdir("/etc")` lists `aether`; `vfs_readdir("/etc/aether")` lists
   `config` — hierarchy is enumerable at each level.
Prints `[sfs] hier dirs OK` (gate sentinel), `hier dirs FAIL` forbidden. Runs on
the existing SFS blk2 (`smoke-fs-sfs-rw`'s disk); the kernel formats it in place.

Regression: `smoke-fs-sfs-rw` (root-level SFS create/lookup must still pass —
single-component paths are the 1-deep case of the walk), `smoke-fs`,
`smoke-user` (ELF load from SFS root is unchanged), then the full suite.

## Non-goals (explicit follow-on slices)

- **Not** the process-root switch: `root_mnt` stays FAT; user `SYS_OPEN` is
  unchanged. Moving `/AETHER.CFG` to `/etc/aether/config` waits for that slice.
- No `rmdir`/directory `unlink` (files only, as today); no `..`/`.` entries; no
  hard links. A directory is removed only by reformatting for now.
- No host `mkfs.sfs` — the kernel provisions the tree at boot (as it already
  does for embedded ELFs).
