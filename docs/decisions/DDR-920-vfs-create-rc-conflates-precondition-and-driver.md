= DDR-920 — `vfs_create`'s `-1` conflates a precondition failure with a driver failure

**Status:** ACCEPTED. Diagnosability fix for the `btree churn FAIL` intermittent.
**Date:** 2026-08-15
**Lineage:** DDR-884 (keep the churn rc) → **DDR-920 (this)**.
Related: DDR-917 / DDR-918 (same defect class — one message, several causes).
**Not related to** DDR-919: the `g_ticks` freeze is fixed and confirmed; this is
a different intermittent that the freeze previously masked.

## Evidence

CI run 31845930664, shard 4. `smoke-rtc-smp` failed as collateral (DDR-785:
a foreign probe FAIL fails whatever gate is booting):

```
[smoke] FAIL — a probe reported 'btree churn FAIL' during this gate's boot.
[smoke]   [sfs] churn FAIL op=create iter=0 rc=-1
[smoke]   [sfs] btree churn FAIL
shard 4: FAILED at smoke-rtc-smp after 2 of 21 gates
```

DDR-884's instrument did its job: we now know it is `op=create`, at `iter=0`,
with `rc=-1`. That is as far as the current code can tell us, and it is not far
enough.

## What `rc=-1` actually means — and what it rules out

`vfs_create` (kernel/fs/vfs/vfs.c:125-134):

```c
int vfs_create(cap_t cap, int mnt, const char *path, struct vfs_file *out) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->create || !cap_ok(cap, CAP_FS_WRITE))
        return -1;                       /* (A) precondition */
    mnt_lock(m);
    int r = m->fs->create(m->ctx, path, out);
    mnt_unlock(m);
    if (r == 0) out->mnt = mnt;
    return r;                            /* (B) driver passthrough */
}
```

`-1` can arrive from **(A)** — an invalid mount id, a filesystem with no
`create` op, or `CAP_FS_WRITE` denied — or from **(B)**, if the SFS driver
itself returns `-1`. These are completely different bugs and they are currently
indistinguishable.

**It also rules out DDR-884's three stated candidates.** `-EEXIST` is -17,
`-ENOSPC` is -28, and an ADR-032 budget refusal is neither; none of them is
`-1`. So the leftover-file / full-volume / rate-limit hypotheses are **not**
what this failure is, and should not be pursued on this evidence.

That points at **(A)** — most plausibly `mnt_get(mnt)` returning NULL because
`root_smnt` is not a valid mount at that instant, or `cap_ok` denying
`CAP_FS_WRITE`. Failing at `iter=0`, the very first create, is consistent with a
precondition that was never satisfied rather than with state accumulated by
churn. **Plausible, not confirmed** — which is exactly why the codes must be
split before anything is "fixed".

## Decision

Give branch (A) distinct return codes so the next occurrence names itself:

- `-EPERM` when `cap_ok(cap, CAP_FS_WRITE)` fails — a capability problem.
- `-EINVAL` when the mount is missing or has no `create` op — a mount problem.

Branch (B) still passes the driver's own return through unchanged.

### Why this is safe

All 20 in-tree callers of `vfs_create` test `== 0` / `!= 0`; **none** compares
against `-1` specifically (verified by grep). Widening the negative space
therefore changes no control flow. The churn probe prints whatever it receives,
so it needs no change to benefit.

### Scope

`vfs_read`, `vfs_write` and the other VFS entry points share the same
`return -1` precondition shape. They are **deliberately left alone in this
slice**: `op=create` is the confirmed failing operation, and changing five
functions to chase one confirmed failure is how unrelated regressions get
introduced. Follow-on work, once this one is confirmed by a capture.

## What this does NOT do

It does not fix the churn failure. It makes the next occurrence say which of two
unrelated defects it is. Per the standing rule, the fix follows the evidence —
and the evidence does not yet exist.
