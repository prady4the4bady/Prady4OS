# DDR-769 — nested-directory provisioning in host `mkfs.sfs`

**Status:** implemented — `smoke-sfs-persist` extended, PASS (kernel reads a
host-authored `/etc/aether/config` → `PRADYOS_SFS_NESTED_OK`, alongside
`PRADYOS_SFS_PERSIST_OK`). Host round-trip + image both build clean. Follows
DDR-767 (mkfs.sfs) / DDR-768 (kernel reads the host image).

## Problem

`mkfs.sfs` (DDR-767) provisions only root-level files. Real provisioning targets
live at nested paths — notably `/etc/aether/config`, today created by the kernel
at boot on the persistent SFS root (DDR-760/761). To move that off the kernel and
into a build-time image, mkfs must write directory hierarchies the kernel
traverses.

## On-disk facts (confirmed from `sfs.c`)

- `sfs_walk` splits a path on `/`, resolving each intermediate component as a
  directory and taking the last as the leaf. Each component is looked up by
  `(parent_inode << 32) | FNV1a32(name)` (a DIR-keyed B+tree slot → child inode).
- A directory inode is a normal inode block with `flags = SFS_INO_DIR`
  (`== SFS_I_DIR == 1`) and **no data extents** — its contents ARE the DIR-keyed
  slots whose parent is that inode. (Matches `sfs_do_create(..., is_dir=1)`.)
- The linear leaf scan means all slots (any parent) can live in the single root
  leaf as long as the total ≤ `SFS_LEAF_MAX = 14`. `/etc/aether/config` needs 7
  slots (root+etc+aether+config INODE ×4, etc+aether+config DIR ×3).

## Decision — `tools/mkfs_sfs/mkfs_sfs.c` (a: nested provisioning + gate)

Replace root-only `add_root_file` with a path-aware `add_file(path, data, len)`:

1. Skip leading `/`, split `path` on `/`. Walk components under a running parent
   (start = `SFS_ROOT_INODE`). For each **intermediate** component call
   `find_or_make_dir(parent, name)`: scan the leaf for an existing DIR slot
   `(parent<<32)|hash(name)`; reuse its inode if present, else allocate a dir
   inode block (`flags = SFS_INO_DIR`, `extent_count = 0`) + append its INODE and
   DIR slots; return the child inode. This dedups shared prefixes across multiple
   `--file` args (e.g. `/etc/a` and `/etc/b` share `/etc`).
2. The **final** component is the file (existing inode/inline-extent path), its
   DIR/INODE slots keyed under the final parent.
3. Keep the single-leaf model; error out (not truncate) past `SFS_LEAF_MAX`.

Extend `sfs_readback` the same way (walk components) so the host round-trip gate
covers nested paths.

## Gate

Extend `smoke-sfs-persist` (DDR-768): mkfs provisions `/etc/aether/config` (a
known marker) alongside `/PERSIST.TXT`; the kernel persist self-test also
`vfs_open`/`vfs_read`s `/etc/aether/config` → `PRADYOS_SFS_NESTED_OK`. Proves the
kernel traverses host-authored directory hierarchies.

## Non-goals / follow-on (DDR-770 = "b")

- **Migrating the persistent-root `/etc/aether/config` off kernel provisioning**
  (DDR-760/761) onto a shipped mkfs image is deferred: it touches the boot/root
  mount flow and the destructive SFS self-tests that reformat blk2, so it wants
  its own slice once nested provisioning is proven here.
- Multi-leaf trees (>14 total slots → real B+tree splits on the host); large
  directory fan-out. Still single-leaf this slice.
