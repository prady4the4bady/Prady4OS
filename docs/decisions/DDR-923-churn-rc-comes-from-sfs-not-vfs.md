= DDR-923 — the churn `rc=-1` comes from SFS, not from `vfs_create`'s precondition (corrects DDR-920)

**Status:** ACCEPTED (diagnosis + instrument). **Corrects an inference in DDR-920.**
**Date:** 2026-08-15
**Lineage:** DDR-884 (keep the rc) → DDR-920 (split vfs_create's -1) → **DDR-923 (this)**.

## What DDR-920 inferred, and why it was wrong

DDR-920 split `vfs_create`'s bare `-1` into `-EPERM` (capability) and `-EINVAL`
(mount), and reasoned:

> That points at **(A)** — most plausibly `mnt_get(mnt)` returning NULL … or
> `cap_ok` denying `CAP_FS_WRITE`.

**That inference is refuted by the code.** It rested on `-1` matching none of
DDR-884's candidate errnos, which is true but does not imply the precondition
branch — it only implies "not those errnos".

## Answering TASK B's three questions from the code

**Q2 — does SFS register a `.create` op?** YES.
`kernel/fs/sfs/sfs.c:1492-1496`: `static const struct vfs_fs_ops sfs_ops = { …
.create = sfs_create, … }`. So the "no create op" half of `-EINVAL` is
impossible.

**Q1 — is the mount id valid at `iter=0`?** YES.
`kernel/main.c:1937`: `int root_smnt = vfs_mount(2);` guarded immediately by
`if (root_smnt >= 0)`. Between that line and the churn at `main.c:2042` there is
**no `vfs_unmount`, no remount, and no reformat** (checked by scanning the whole
range). Decisively, the *same* `cap` and the *same* `root_smnt` are used to
create `/etc/aether/config` successfully at `main.c:1950`, inside the same
guarded block, before the churn runs.

**Q3 — does the churn hold `CAP_FS_WRITE`?** YES, necessarily.
The churn is not a separate thread with its own mask; it runs inline in
`fs_test_thread` and passes the same `cap` variable that just succeeded at
line 1950. A capability that satisfies `cap_ok(cap, CAP_FS_WRITE)` for one
`vfs_create` satisfies it for the next.

**Therefore neither `-EPERM` nor `-EINVAL` can be the observed `rc=-1`.**

## Where the `-1` actually comes from

`sfs_create` (`kernel/fs/sfs/sfs.c:689-696`) returns a bare `-1` itself:

```c
static int sfs_create (void *ctx, const char *path, struct vfs_file *out) {
    if (sfs_walk(c, path, 1 /* mkdir -p intermediates */, &parent, &name, &len) != 0)
        return -1;                      /* (i)  path walk failed          */
    if (sfs_do_create(c, parent, name, len, 0 /* file */, &ino) != 0)
        return -1;                      /* (ii) node creation failed      */
    …
```

with a third at `:711`. So the churn's `rc=-1` is `vfs_create`'s **branch (B)**,
the driver passthrough — the value is SFS's, and it arrives through the VFS
untouched.

**Consequence for DDR-920:** the split is still correct and worth keeping (it
disambiguates the VFS layer permanently, and cost nothing), but it will **not**
name this failure. The next capture will still print `rc=-1`. Recording that
now, so the next session does not wait on a capture that cannot speak.

## The real ambiguity, one layer down

`sfs_create` collapses at least three distinct outcomes into one `-1`:

| site | meaning |
|---|---|
| `sfs.c:693` | `sfs_walk` failed — parent path could not be resolved/created |
| `sfs.c:696` | `sfs_do_create` failed — no free inode/extent, or the name exists |

(An earlier draft of this DDR listed `sfs.c:711` as a third site. That is wrong:
:711 is inside `sfs_dir_walk`, a different function. `sfs_create` has exactly
two. Corrected before commit.)

This is the same defect class as DDR-917, DDR-918 and DDR-920: one message,
several causes. It is why `op=create iter=0` has been unactionable for three
sessions.

## Decision

Give `sfs_create`'s failure paths distinct return codes, so the existing DDR-884
instrument prints something that names the cause:

- `-ENOENT` when `sfs_walk` cannot resolve/create the parent path.
- `-ENOSPC` when `sfs_do_create` fails (the only plausible resource failure at
  `iter=0` on a freshly formatted volume) — and, where the driver can already
  distinguish "name exists", `-EEXIST`.

Callers test `!= 0` (the churn probe prints whatever it gets), so widening the
negative space is safe by the same argument DDR-920 verified for `vfs_create`.

## What this does NOT do

It does not fix the churn failure. `iter=0` on a fresh volume failing to create
one file is not explained by any of the three, which is exactly why the code
must say which one it is before a fix is written. **Do not touch the B+tree, the
churn loop, or `sfs_do_create`'s internals until a capture names the branch.**
