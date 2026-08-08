# DDR-867 — ftruncate ships (item 20), and DDR-866's "bug" was my harness

**Status:** Accepted — **supersedes DDR-866**
**Date:** 2026-08-08
**Scope:** Group 3 item 20. Kernel + probe + gate.

## The correction, first

DDR-866 reverted a working implementation. The "deterministic data-corruption
bug" — 5 runs, 5 failures, `shrink lost or altered surviving content` — **was
not in the kernel. It was in my verification harness.**

`mirror_build.sh` used `rsync -a`, which **preserves the source mtime**. So
restoring a mutated file whose mtime was older than the object already built
from it left `make` believing that object was current. The "restored" tree kept
running the **mutated** code. The failure message was mutation M3's exact
symptom (`keep = 0`), which is precisely what it was still executing.

A clean-room rebuild — `rm -rf` the mirror, full build from scratch — passes
**5/5**. The implementation was correct when I reverted it.

This is the third time in this project that a stale artifact made a verification
harness lie (after DDR-853's `__pycache__` and DDR-862's flags-only parity), and
the lesson is the same each time: **a restore that does not force a rebuild has
not restored anything.**

The harness now uses `rsync -rlpgoD --checksum --no-times`: files are selected
by content, and the ones that change are stamped with *now*, which is newer than
any object, so `make` rebuilds exactly those and nothing else. Incremental
speed, no staleness.

**What I got wrong in judgement, not just in tooling:** I treated 5/5 determinism
as proof the bug was real, when determinism only shows the *input* was constant —
and a stale object is a very constant input. The right check was the one I ran
afterwards and should have run first: wipe everything and rebuild.

## What ships

- `vfs_fs_ops.truncate`, appended. fat32 and ext4 do not set it, so it is NULL
  and `vfs_truncate` refuses rather than calling through a garbage pointer.
- `vfs_truncate`, gated on **`CAP_FS_WRITE`** — truncate destroys content
  exactly as a write does, and without that gate it is a delete primitive that
  bypasses the write check. Deliberately **not** charged to the ADR-032 token
  bucket: that bounds write *throughput*, and a truncate moves no caller bytes.
- `sfs_truncate` — read-then-rewrite, because an LZ4 extent cannot be trimmed
  without decompressing it anyway. Bounded at 64 KiB and **refuses** past it
  rather than satisfying the call at a length nobody asked for.
- `SYS_FTRUNCATE` at NSI 94, negative length checked **signed before the cast**.
- `user/ftrunctest.c` + `smoke-ftruncate`, probe-gated via DDR-804 because the
  probe writes to the shared SFS root.

## The second DDR-866 finding, now fixed

The probe asserted only "returns negative" for a negative length. With the sign
check removed, `(uint64_t)-1` still failed — `sfs_truncate`'s 64 KiB bound
rejects it — returning `-EIO`. Both are negative, so the assertion passed while
the guard it claimed to test was gone. The probe now pins **`-EINVAL`
specifically**, and mutation M2 kills it.

## Verification

Mutation table, re-run on the fixed harness. Every row behaves:

| arm | expected | result |
|---|---|---|
| baseline | PASS | ✅ |
| grow stops zero-filling | FAIL | ✅ `grow left stale bytes instead of zeros` |
| negative length not refused | FAIL | ✅ `negative length not -EINVAL` |
| shrink keeps nothing | FAIL | ✅ `shrink lost or altered surviving content` |
| restore | PASS | ✅ |

Regression set, all green: `smoke-ftruncate`, `smoke`, `smoke-fsrm`,
`smoke-fs-sfs-rw`, `smoke-fs-rw`, `smoke-sfs-persist`, `smoke-user`.
Clean-room run: 5/5. Zero warnings under `-Werror`.
`shard-check`: 135 gates across 6 shards.

**Group 3 item 20 is complete.**
