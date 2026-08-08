# DDR-866 — ftruncate (item 20): built, gated, and REVERTED on a real bug

**Status:** Work in progress — **not merged**
**Date:** 2026-08-08
**Scope:** Group 3 item 20. Kernel change reverted; the probe is parked at
`docs/wip/ftrunctest.c.wip`.

## What was built

`ftruncate` did not exist anywhere in the kernel, so this was a full slice:

- `vfs_fs_ops.truncate`, **appended** to the op table. fat32 and ext4 simply do
  not set it, so it is NULL for them and `vfs_truncate` refuses rather than
  calling through a garbage pointer.
- `vfs_truncate`, gated on **`CAP_FS_WRITE`** — truncate destroys content
  exactly as a write does, and a caller able to zero a file without the write
  capability would have a delete primitive that bypasses the write gate.
  Deliberately **not** charged to the ADR-032 token bucket: that bucket bounds
  write *throughput*, and a truncate moves no caller bytes.
- `sfs_truncate` — read-then-rewrite, because an extent may be LZ4-compressed
  and cannot be trimmed without decompressing it anyway. Bounded at 64 KiB and
  **refuses** beyond it rather than truncating to what fits: a "successful"
  truncate that produced a different length than asked for is the worst outcome
  here, because the caller's next write lands somewhere it does not expect.
- `SYS_FTRUNCATE` at NSI 94, with the negative length checked as **signed before
  the cast** — casting first turns `-1` into a request to grow to 16 exabytes.
- `user/ftrunctest.c` + `smoke-ftruncate`, probe-gated via DDR-804 fw_cfg
  because the probe writes to the **shared SFS root**; running it every boot
  would change what every other SFS gate observes.

It compiled clean under `-Werror`, and `smoke`, `smoke-fsrm` and
`smoke-fs-sfs-rw` all passed alongside it.

## Why it is reverted

**The gate caught a real bug in my own implementation.** Mutation-testing the
gate, then re-running the clean tree, produced a deterministic failure — 5 runs,
5 failures, same message:

```
FTRUNC FAIL: shrink lost or altered surviving content
```

The size after shrink is correct (16), but the surviving **bytes are wrong**. So
`sfs_truncate`'s read-then-rewrite path preserves length and corrupts content.
The cause is not yet identified; the plausible candidates are the partial read
from a compressed extent, or the re-write of a buffer whose tail is zero-padded
to 64 KiB.

Committing it would have put a silent data-corruption path into the filesystem.
The project's rule is that a slice ships when it is correct, not when it builds,
so the kernel change is reverted and only this record and the probe are kept.

## What the gate proved, which is worth keeping

The mutation table below was run against the *working* build, and it is the
reason the bug was found at all rather than shipped:

| mutation | gate verdict | message |
|---|---|---|
| grow stops zero-filling | **FAIL** | `grow left stale bytes instead of zeros` |
| shrink keeps nothing | **FAIL** | `shrink lost or altered surviving content` |
| negative length no longer refused | **PASS** ← gap | — |

The third row is a second finding. Removing the sign check did **not** fail the
gate, because `sfs_truncate`'s 64 KiB bound rejects the huge value anyway and
the probe only asserts "returns negative". `-EIO` and `-EINVAL` are both
negative, so the probe cannot tell them apart. **The sign check is therefore
untested**, and my comment claiming it "is the whole guard" overstated it — the
size bound is doing the work. When this is redone, the probe must assert the
specific errno.

## Next session

1. Find the content-corruption bug — instrument `sfs_truncate` to dump the
   buffer before `write_extent`, which separates "read gave wrong bytes" from
   "write stored wrong bytes".
2. Tighten the probe to assert `-EINVAL` specifically.
3. Re-run the mutation table; all three rows must fail.
