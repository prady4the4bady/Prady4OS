# DDR-1184 — `fd_clone` out-of-memory must not leave the child holding the parent's file

**Status:** ACCEPTED (bug fix; Order 4, #45/6301435736: decisions delegated).
**Security boundary: review before merge.** The defect frees a live kernel object that another process still uses.

## 1. The defect, measured on the tree at `dev/phase1` = `bab06f4`

`fd_clone` (`kernel/proc/fd.c:72-100`) copies the parent's fd table into a new child at fork (`sched.c:1335`). For each slot it does:

1. `*de = *se;` (`:78`). The child's slot now holds **the parent's `vfs_file` pointer**.
2. For an `FD_VFS` slot, `kmalloc` a private copy (`:86`) and only then overwrite `de->file` (`:96`).

If the `kmalloc` at slot `i` fails:

- the rollback (`:88-92`) frees the copies of slots `j < i` only, and returns `-ENOMEM`;
- slot `i` of the child still holds the parent's pointer from step 1;
- the caller destroys the child: `sched_destroy` → `sched_free_tcb` → `fd_free` on every slot (`sched.c:1410-1411`). `fd_free` (`fd.c:59-60`) does `kfree(e->file)` on slot `i`, which **frees the parent's open file**.

The parent keeps the pointer. `KHEAP_DEBUG` (always on, `kheap.c:20`) poisons the freed object with `0xDD`. The parent's next `read`/`write`/`lseek` then uses poisoned memory. Its `close` (or exit) frees the object a second time, and `kfree` panics with `double free` (`kheap.c:171`). If the slab slot is reused first, the parent's fd instead aliases some other kernel object. In both cases an allocation failure in one process damages another process's state.

How it is reached: any `fork` where the kernel heap cannot supply one `struct vfs_file` (24 bytes, so the 32-byte size class, `kheap.c:61`). A full slab with no free PMM page for a new one is enough. The parent needs only one open `FD_VFS` fd.

`sched_create_user_thread` on the P1c branch (#76, `sched.c:1389`) calls `fd_clone` the same way and inherits the defect. It also inherits the fix when this merges forward.

## 2. Decision

**A child slot is written only after its private copy exists.** For an `FD_VFS` slot, `fd_clone` allocates and fills the copy first. Only then does it write the slot, already pointing at the copy. On `-ENOMEM`, slot `i` and every later slot are untouched: they still hold what `sched_create_state` gave them (`FD_NONE`, or the console for 0-2). They can never hold a parent pointer.

The rest is unchanged:

- The existing rollback of the copies `j < i` stays.
- Pipe increfs on slots `j < i` stay for the caller's `fd_free` to drop. That is how fork's failure path already balances them.

The function's contract is written at its head: on failure, `dst` holds only references it owns, and the caller releases them with `fd_free` on every slot.

**Test seam.** `fd_clone` becomes a one-line wrapper around a static `fd_clone_with(src, dst, alloc)`, and `fd_clone` passes `kmalloc`. A boot self-test in `fd.c` passes an allocator that fails on a chosen call. This choice has these properties:

- no global flag, no build flag and no rebuild;
- production pays one indirect call per `FD_VFS` slot at fork;
- nothing in the fork path changes behaviour except the ordering above.

## 3. Gate `smoke-fdclone`

`fd_clone_selftest()` runs once at boot, beside the other self-tests before `smp_start_aps()` (`main.c`), on one CPU, so no fork can race it. It works on two `kmalloc`'d `struct tcb` buffers (only `fdt` is touched) and runs these steps:

1. **Parent setup.** It gives the parent three `FD_VFS` slots, 3, 4 and 5. Each holds a `vfs_file` filled with the byte `0x5A + slot`.
2. **Baseline.** It records `kheap_outstanding()`.
3. **Injected failure.** It clones with an allocator that succeeds once (slot 3) and then fails (slot 4).
4. **Child teardown.** It calls `fd_free` on every child slot, which is exactly what `sched_destroy` does.
5. **Checks.** It verifies that each parent file still holds its fill byte, and compares `kheap_outstanding()` with the baseline.
6. **Parent cleanup.** It frees the parent's files only if they are intact. If one was already freed, the test prints a FAIL line and leaks it rather than double-freeing into a panic.

| Arm | Exact line | Meaning | On the unfixed tree |
|---|---|---|---|
| A | `PRADYOS_FDCLONE_A enomem=1 parent_intact=1` | the clone reported `-ENOMEM`, and child teardown left every parent file intact | `parent_intact=0`: slot 4's teardown frees and poisons the parent's file. **Red.** |
| B | `PRADYOS_FDCLONE_B leak=0` | after the child is torn down, the heap holds exactly what it held before the clone | `leak=1 delta=…`: one object fewer than the baseline, because the parent's object was freed by the child. **Red.** |

`FDCLONE FAIL` is forbidden. Both arms are red on the unfixed tree, so neither is vacuous.

Mutants (each run in CI and shown red before merge):

| Mutant | Change | Arm it must turn red |
|---|---|---|
| M1 | the old order: `*de = *se` before the allocation (the defect itself) | A |
| M2 | the rollback clears `dst->fdt.e[j].file` without `kfree` (the copies leak) | B only (A stays green: the parent is untouched) |

### 3.1 Mutant runs (CI evidence, mutants built on `85921a4`)

Both runs failed only `smoke-fdclone`, the last of 19 gates in shard 8. The other 18 passed.

| Mutant | Run | Serial lines | Result |
|---|---|---|---|
| M1 | [40124103817](https://github.com/prady4the4bady/Prady4OS/actions/runs/40124103817) | `PRADYOS_FDCLONE_A FAIL`, `PRADYOS_FDCLONE_B leak=1 delta=0xFFFFFFFFFFFFFFFF`, `FDCLONE FAIL: child teardown freed a parent file` | **red on A** (and B: the heap is one object *below* its baseline, because the child freed the parent's file) |
| M2 | [40124105794](https://github.com/prady4the4bady/Prady4OS/actions/runs/40124105794) | `PRADYOS_FDCLONE_A enomem=1 parent_intact=1`, `PRADYOS_FDCLONE_B leak=1 delta=0x0000000000000001` | **red on B only** |

## 4. Honesty labels

- Gated by `smoke-fdclone` arms A-B, which cover the fork path's `fd_clone`. Emulated (QEMU TCG) only, not verified on real hardware.
- Out-of-memory at fork has not been observed in a CI log. The defect is found by reading the code, and the gate shows it with an injected failure.
- Does not touch U1-U3 or OPEN-1/2/12/13.
