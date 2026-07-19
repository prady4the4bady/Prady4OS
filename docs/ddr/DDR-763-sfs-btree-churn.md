# DDR-763 — SFS "B+tree bug": misdiagnosis corrected + churn regression gate

**Status:** accepted (investigation + regression coverage; no B+tree change)
**Layer:** fs (SFS) diagnosis + boot self-test. M2 storage.

## Problem (as handed off)

The prior session flagged a HIGH-PRIORITY "correctness-critical SFS B+tree bug":
repeated `create + write(64K) + unlink` of the same path failed the WRITE at the
~11th cycle (`failop=2`), and — because that cycle count is near `SFS_LEAF_MAX=14`
— it was attributed to the B+tree leaf-split/`bt_insert_rec` path. Free-space GC
(DDR-762) was declared blocked on it.

## Investigation (reproduction-first, per protocol)

Built an instrumented churn harness on the persistent SFS root and put a distinct
marker at each of `sfs_write`'s four `return -1` paths. Result: `reached=10,
failop=2`, but **none of the `sfs_write` markers fired** — so `sfs_write` was
never reached. The write failed *earlier*, inside `vfs_write`:

```c
// kernel/fs/vfs/vfs.c
if (current_thread->fs_write_budget < len)   // FS_WRITE_BUDGET_DEFAULT = 1 MiB
    return -1;                                // returns BEFORE calling the FS
```

**Root cause: the 1 MiB per-thread FS write budget, not the B+tree.** The boot
thread writes every embedded user ELF to SFS during boot (`user_boot_from_sfs`;
~20 ELFs incl. the ~100 KB+ musl PRISM/AETHER daemon) plus the FS self-tests,
consuming most of its 1 MiB. Only ~10 further 64 KB writes fit → the churn fails
at ~cycle 11. The ~14-slot leaf split was a **coincidence**, not the cause.

**Proof:** refreshing the boot thread's `fs_write_budget` before the churn makes
it reach **40/40 cycles, `failop=0`** — the B+tree correctly handles growth far
past its first leaf split. There is **no B+tree bug**; `kernel/fs/sfs/sfs.c` is
unchanged by this slice.

## Decision

1. **No B+tree fix** — the tree is sound (root-cause honesty: do not invent a fix
   for a non-bug).
2. **Add the missing coverage** that let the misdiagnosis happen: a boot self-test
   churns 40× create+write(64K)+unlink on the SFS root (past the leaf split) and
   prints `[sfs] btree churn OK`. It refreshes the boot thread's write budget so
   it exercises the *B+tree*, not the budget (legitimate — kernel self-test
   context, not a userspace consumer).
3. **Correct the handoff**: the SFS-BTREE-BUG finding is retracted; DDR-762
   free-space GC is UNBLOCKED (its earlier iter-10 failure was the same write
   budget — its GC test must likewise refresh the budget to isolate block reuse).

The 1 MiB *lifetime* per-thread write budget (anti-DoS, `sched.h`
`FS_WRITE_BUDGET_DEFAULT`) is noted as a separate design limit — restrictive for a
process that legitimately writes many files on the persistent SFS root — but it is
a designed security limit, not a bug, and re-scoping it (higher / refillable /
per-op) is its own future decision, out of scope here.

## Gate — `smoke-sfs-btree` (new; 96 → 97)

`EXTRA_SENTINEL='[sfs] btree churn OK'`, `FORBIDDEN='btree churn FAIL'`, via
`boot_test.sh`. Proves the B+tree survives 40 same-path create/write/unlink cycles
(past `SFS_LEAF_MAX`). Regression: full SFS suite (`fs-sfs-rw`/`sfs-dirs`/
`sfs-unlink`/`sfsroot`) + `aethercfg` unchanged (no SFS code change).

## Non-goals

- No change to `sfs.c` (no bug). No write-budget re-scoping (separate decision).
- Free-space GC (DDR-762 redo, free-extent-run allocator) is the next slice.
