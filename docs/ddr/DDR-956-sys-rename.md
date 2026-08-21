# DDR-956 — `SYS_RENAME` + PRISM `mv`

Status: DESIGN. Written before code (R16).
Supersedes the FEAT-B sketch in three places, each verified against the tree.

## 1. What is being added

- `SYS_RENAME` = **NSI 95**. Measured, not assumed: the highest assigned number
  in `kernel/syscall/syscall.h` is **94** (`SYS_FTRUNCATE`), and 95 is free.
  (79/80/81 are `SYS_GOAL_SIGN`/`SYS_GOAL_VERIFY`/`SYS_ACC_ROTATE` — not free,
  despite earlier notes calling 79 the next free number.)
- Capability: **`CAP_FS_WRITE`**, same as `vfs_unlink`.
- Semantics: `rename(old_path, new_path)` within the calling process's root
  mount. Atomic: one journal transaction carries both key changes.

## 2. THREE CORRECTIONS to the prescribed plan (R12)

### 2a. `-EXDEV` is UNREACHABLE in this kernel — the arm must be dropped
The plan calls for `vfs_rename` to "resolve old_path to a mount, resolve
new_path to a mount, if mounts differ return -EXDEV", and for a gate sentinel
`PRADYOS_RENAME_CROSS_MNT` asserting `EXDEV`.

**There is no path→mount resolution anywhere in this tree.** Every VFS entry
point takes an explicit mount id (`vfs_unlink(cap, mnt, path)`,
`vfs_open(cap, mnt, path, …)`), and every syscall passes exactly one:
`t->root_mnt` (`sys_file.c:29,46,49,61,176`). A process sees a single
filesystem rooted at `root_mnt`; it cannot name a second mount.

So no caller can construct a cross-mount rename, and an `EXDEV` branch in
`vfs_rename` would be **dead code**, which CLAUDE.md §3 forbids. A gate arm
asserting `EXDEV` could then only pass by the probe fabricating the string,
which is a sentinel that fires regardless of kernel behaviour — the exact
anti-pattern R8 exists to prevent.

**Replacement third arm: `PRADYOS_RENAME_ENOENT`** — renaming a path that does
not exist must fail, not silently succeed. This is reachable, is on the §4 edge
list, and is genuinely discriminating: a stub returning 0 passes arms 1 and 2 in
a trivial implementation but fails this one.

### 2b. `sfs_rename` must NOT take the mount lock — that would deadlock
The plan says `sfs_rename` should "take the mount lock (mnt_lock_live pattern
from vfs)". The layering is the opposite: `vfs_unlink` takes the lock and then
calls `m->fs->unlink(m->ctx, path)` (`vfs.c:231-233`), and `sfs_unlink` is
`static int sfs_unlink(void *ctx, const char *path)` — it receives `ctx` with
the lock **already held** and never touches the lock itself.

`mnt_lock` is a sleep-mutex (`__atomic_exchange_n` + `yield()`), so re-acquiring
it on the same thread would spin forever against itself. `sfs_rename` therefore
takes no lock; `vfs_rename` takes it exactly as `vfs_unlink` does.
(Same class of error as the FEAT-A lock-contract inversion caught in DDR-955.)

### 2c. Two file paths in the plan do not exist
| plan | tree |
|---|---|
| `kernel/include/syscall.h` | `kernel/syscall/syscall.h` |
| `kernel/syscall/sys_fs.c` | `kernel/syscall/sys_file.c` |

## 3. Files to modify
- `kernel/fs/vfs/vfs.h` / `vfs.c` — `vfs_rename` (mnt_lock_live pattern)
- `kernel/fs/sfs/sfs.c` — `sfs_rename` (journal tx; no locking)
- `kernel/syscall/syscall.h` — `SYS_RENAME 95`
- `kernel/syscall/sys_file.c` — handler, modelled on `sys_unlink`
- `user/prism.c` — `mv` builtin, modelled on `rm`
- `user/renametest.c` — probe, **zero writable globals** (DDR-826)
- `Makefile`, `tools/ci/gate_shards.txt` — build + register `smoke-rename`

## 4. Atomicity — the shape copied from `sfs_unlink`
`sfs_unlink` ends with a tombstone write then a single `sfs_commit(c)`
(`sfs.c:1150-1156`): set `s.v.dir.inode_num = 0`, `bt_insert`, `sfs_commit`.

`sfs_rename` performs **two** `bt_insert` calls — insert the new key pointing at
the SAME inode number, then tombstone the old key — followed by **one**
`sfs_commit`. Both key changes land in a single journal transaction, so a crash
replays both or neither. Never copy-inode-then-unlink: that has a window where
the file is duplicated, and a crash between the halves leaves stale data.

## 5. Gate — `smoke-rename` (shard 4, 60 s)
Required: `PRADYOS_RENAME_OK`, `PRADYOS_RENAME_OLD_GONE`, `PRADYOS_RENAME_ENOENT`.
Forbidden: `[vblk] compl wait timeout`, `[vblk] slot wait timeout`.

Note the forbidden sentinels make the gate ineligible for DDR-785 early exit, so
it burns the full window; 60 s is budgeted accordingly. They are kept anyway —
BUG-1 is still open and a timeout firing here is information worth failing on.

## 6. Edge cases the implementation must handle
- `old == new`: no-op, return 0
- `new` exists: replaced (Unix semantics), inside the same transaction
- `old` absent: error (drives the `ENOENT` arm)
- directory rename: allowed only if `new` is absent or an empty directory,
  reusing `sfs_is_dir` + `sfs_dir_walk` as `sfs_unlink` does
- prefix collisions (`/a/bc` vs `/a/b`) are distinct keys — the B+tree key is
  `(parent_ino << 32) | FNV1a32(name)`, so no prefix aliasing exists

---

## 7. GATE BLOCKED — PRISM is FAT-rooted and fat32 has no rename op

The PRISM-shell gate strategy was implemented (two gates, 147/6/6, Makefile
parsing clean) and **failed on first run** with the rename itself failing:

```
prism> mv: cannot rename /RENSRC.TXT
prism> cat: cannot open /RENDST.TXT
```

Cause, confirmed in the tree:
- `fat32_ops` declares `.unlink` but **no `.rename`** (`fat32.c:635`), so
  `vfs_rename` returns `-ENOSYS`.
- PRISM is launched via `user_boot_from_sfs(cap, smnt, "PRISM.ELF", …)`
  (`main.c:1934`), which loads the ELF *from* SFS but does **not** set
  `root_mnt` — the comment at `main.c:1831` says so explicitly ("root_mnt is set
  BEFORE unblock, which user_boot_from_sfs doesn't allow"). PRISM therefore runs
  on the FAT default root.

### Consequence worth stating plainly
`mv` is shipped but **non-functional for every shell user**: the syscall and the
SFS backend are correct, but the filesystem PRISM actually runs on cannot rename.

### Why the ENOENT gate was ALSO withdrawn
It would have passed — vacuously. On FAT every `mv` fails, so
`mv: cannot rename …` appears regardless of whether rename is implemented
correctly. A sentinel that fires independently of the behaviour under test is
exactly what R8 forbids, and shipping it would have recorded a green for a
feature that does not work.

### Two ways to unblock, both out of this task's scope
1. **Implement `fat32_rename`.** Bounded but not trivial: `fat32_unlink`
   (`fat32.c:599-617`) resolves the parent, scans the entry, frees the chain and
   stamps `0xE5`. A rename overwrites the 11-byte 8.3 name in place for a
   same-directory move — but any file with a long-name chain has LFN entries
   preceding it, and renaming only the 8.3 record leaves that chain pointing at
   the new name. Getting that wrong corrupts the directory, so it needs its own
   DDR rather than being appended here.
2. **Root PRISM at SFS** — that is FEAT-E (SFS as default process root), already
   a separate queued item. It would make `mv` work for shell users immediately
   and make this gate strategy viable unchanged.

Gates reverted; shard matrix back to 145/6/6. `SYS_RENAME` remains implemented,
callable, and **not claimed as shipped**.
