# DDR-1182 — File-descriptor access mode is enforced (closes the DDR-1171 §7 gap)

**Status:** ACCEPTED (Order 3, #45/6282431738, item 1: the `sys_open` access-mode question from #51 is decided here).
**Security boundary: review before merge.** This changes what an fd may do.

## 1. The defect, measured on the tree at `dev/phase1` = `a2ac698`

- `sys_open` (`kernel/syscall/sys_file.c:26-104`) never inspects the access mode. The kernel defines no `O_RDONLY`/`O_WRONLY`/`O_RDWR`/`O_ACCMODE` at all (`sys_file.h:7-9` defines only `O_CREAT`, `O_TRUNC`, `O_APPEND`).
- `sys_open` stores the raw flags in `fd_entry.flags` (`:97-103`, struct at `kernel/proc/fd.h:33-42`). Nothing reads the mode back. As a result, **every `FD_VFS` fd is both readable and writable** whatever it was opened with:
  - `write` → `fd_write_user` (`sys_io.c:115-163`); `writev` goes through the same function (`:315`);
  - io_uring `OP_WRITE` (`sys_io_uring.c:94-100`), which calls `vfs_write` directly;
  - `ftruncate` (`sys_file.c:146-166`);
  - `read` (`sys_io.c:334-358`) and io_uring `OP_READ` (`sys_io_uring.c:112-114`);
  - file-backed `mmap` (`sys_mmap.c:206-213`), which reads the file into a private copy.
- `O_RDONLY|O_TRUNC` on an existing file truncates it (`sys_file.c:77-83`). Only the `CAP_FS_WRITE` check inside `vfs_unlink`/`vfs_create` guards it.
- DDR-1171 §7 found this and worked around it for rollback tasks only (`FD_RB_RO`, `aether.h:388`, checked at `sys_io.c:116` and `sys_io_uring.c:95`). It recorded the kernel-wide gap as not fixed.

So the protection an `O_RDONLY` open is supposed to give (a program cannot change a file it only opened for reading) does not exist today. The capability (`CAP_FS_WRITE`) still bounds which processes can write at all, so this is not an escalation past capabilities. It is a missing second boundary inside a process that holds the capability: a confused deputy that opens read-only can still write.

## 2. Decision

POSIX access-mode semantics, enforced at every fd read/write path, with one helper so that no path can skip it:

- `open`: `acc = flags & 3` (the kernel's own mask; not musl's `O_ACCMODE`, which includes `O_PATH`).
  - `acc == 3` → `-EINVAL`.
  - `O_TRUNC` with `acc == O_RDONLY` → `-EACCES`, and the file is not touched. POSIX leaves this case unspecified; refusing it is the choice that keeps "read-only never modifies".
- **Write paths** (`fd_write_user`, io_uring `OP_WRITE`): `acc == O_RDONLY` → `-EBADF`. This check runs **before** the `FD_RB_RO` check (`-EPERM`) and before the DDR-1170 byte-budget charge, so a refused write costs no budget.
- `ftruncate`: `acc == O_RDONLY` → `-EINVAL` (POSIX allows `EBADF` or `EINVAL`; Linux returns `EINVAL`, which musl callers expect).
- **Read paths** (`read`, io_uring `OP_READ`): `acc == O_WRONLY` → `-EBADF`.
- File-backed `mmap`: the fd must be readable, so `acc == O_WRONLY` → `-EACCES` (POSIX). `MAP_PRIVATE|PROT_WRITE` on an `O_RDONLY` fd stays allowed, because the copy is private and never written back. `MAP_SHARED` file mappings are still `-EINVAL`; when DDR-1178 P4 adds them, `PROT_WRITE` there will require `O_RDWR`.
- `dup2` and fork already copy `flags` (`pipe.c:192`, `fd.c:78`), so a duplicate keeps its mode. The gate checks this.
- Out of scope, recorded:
  - console fds (`FD_CONSOLE`) keep no direction;
  - pipes already enforce direction (`sys_io.c:49-50`, `:361-362`);
  - there is no `fcntl`, so the mode cannot be changed after `open`.

Two helpers live in `fd.h` and are used by every path above: `fd_can_read(e)` and `fd_can_write(e)`. A path that does its own comparison instead of calling them is a review finding.

**Existing callers that must keep working.** `bigwritetest.c:95`, `vaulttest.c:113` and `agent_base.c:229` write through `O_WRONLY` fds. `ftrunctest.c` writes and truncates through `O_RDWR`. No probe writes through a flags-0 fd, and none reads through an `O_WRONLY` one; the survey covered `user/*.c` and `systest.asm`. `smoke-rollback` expects `wexist=-1` (`-EPERM` from `FD_RB_RO`) on an `O_WRONLY` fd, and the new check does not fire there.

## 3. Gate `smoke-accmode` (SFS-rooted probe `user/accmodetest.c`, strict)

For each arm the probe creates its own file (`/ACCA.TXT` to `/ACCH.TXT`) with `O_CREAT|O_WRONLY` and writes 8 bytes. Each arm prints the errno or value the kernel returned:

| Arm | Sentinel | What is checked |
|---|---|---|
| A | `PRADYOS_ACCMODE_A ro_write=-9` | `write` on an `O_RDONLY` fd |
| B | `PRADYOS_ACCMODE_B wo_read=-9` | `read` on an `O_WRONLY` fd |
| C | `PRADYOS_ACCMODE_C ring_ro_write=-9 ring_wo_read=-9` | io_uring `OP_WRITE` on an `O_RDONLY` fd and `OP_READ` on an `O_WRONLY` fd (both CQE results) |
| D | `PRADYOS_ACCMODE_D ro_ftrunc=-22 size=8` | `ftruncate` on an `O_RDONLY` fd, and the file is still 8 bytes |
| E | `PRADYOS_ACCMODE_E acc3=-22 ro_trunc=-13 size=8` | `open` with access mode 3, and `O_RDONLY\|O_TRUNC` on the file (refused, still 8 bytes) |
| F | `PRADYOS_ACCMODE_F dup_ro_write=-9` | `dup2` of an `O_RDONLY` fd, then `write` on the duplicate |
| G | `PRADYOS_ACCMODE_G wo_mmap=-13` | `mmap(PROT_READ, MAP_PRIVATE)` of an `O_WRONLY` fd |
| H | `PRADYOS_ACCMODE_H rw_write=4 rw_read=4` | regression: an `O_RDWR` fd still writes and reads |

`PRADYOS_ACCMODE_DONE` is printed last, and `ACCMODE FAIL` is forbidden. Every arm uses its own 8-byte file, so a mutant that lets one forbidden operation through cannot shift another arm. Writes that should be refused are issued at end-of-file (`lseek(SEEK_END)` first). On the unfixed tree they therefore succeed as appends, rather than failing for an unrelated reason: SFS has no mid-file overwrite (DDR-1089), and that failure would print some other negative. **Vacuity:** on the unfixed tree, A prints `4`, B `8`, C `4 4`, D `0 size=0`, F `4`, and G `32767` (the marker the probe prints for any non-negative mmap result). E prints `acc3=<fd>`, and the file is truncated, so `size=0`. Each of those arms is red today. H passes today; it is a regression arm, recorded as such.

**Mutants. Each lands on a different arm and must be shown red in CI before merge:**

| Mutant | Change | Arm it must turn red |
|---|---|---|
| M1 | `fd_write_user` skips `fd_can_write` | A |
| M2 | `read` skips `fd_can_read` | B |
| M3 | io_uring skips both checks | C |
| M4 | `ftruncate` skips the check | D |
| M5 | `open` accepts access mode 3 and `O_RDONLY\|O_TRUNC` | E |
| M6 | `dup2` widens the copy to `O_RDWR` (`ne->flags \|= O_RDWR`). Clearing the bits instead would be vacuous: an `O_RDONLY` copy with its bits cleared is still `O_RDONLY` | F |
| M7 | `mmap` skips `fd_can_read` | G |
| M8 | `fd_can_write` accepts only `O_WRONLY` (an over-strict fix that refuses `O_RDWR`). A version that refused every write would fail the probe's own setup writes first, so it would not single out H | H |

### 3.1 Mutant runs (CI evidence, head `182c1f8`)

Each run failed on `smoke-accmode` (shard 8) with the missing line shown. The serial value is the one the mutant printed.

| Mutant | Run | Missing line | Printed |
|---|---|---|---|
| M1 | [39500171249](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500171249) | A | `ro_write=4` (F also red: `dup_ro_write=4`) |
| M2 | [39500180510](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500180510) | B | red on arm B |
| M3 | [39500188673](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500188673) | C | `ring_ro_write=4 ring_wo_read=4` |
| M4 | [39500197195](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500197195) | D | `ro_ftrunc=0 size=0` |
| M5 | [39500205562](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500205562) | E | `acc3=3 ro_trunc=3 size=0` |
| M6 | [39500217513](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500217513) | F | `dup_ro_write=4` |
| M7 | [39500226094](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500226094) | D, then G | G: `wo_mmap=32767`. D: `ro_ftrunc=-22 size=PRADYOS_FPU_OK`, a torn line (below) |
| M8 | [39500234320](https://github.com/prady4the4bady/Prady4OS/actions/runs/39500234320) | H | red on arm H |

**Torn-line defect, found by M7.** The probe wrote each arm line in 3 to 5 `write` calls. Another process's output (`PRADYOS_FPU_OK`) landed between two of them, so arm D failed with the right value `-22` and a torn `size=`. On a correct tree the same interleaving would make the gate red at random. Fix: every line is built by `say()` in a stack buffer and leaves in a single `write`. A first version kept the buffer in a static array. It failed CI's DDR-826 check (`ci-probe-rodata-check`: probes may not carry a writable section, `.lbss` 176 B; run 39553352470), so the buffer moved to the stack. This is the same mechanism as the DDR-1181 thread arm F fix. The mutants above ran before this fix; they are re-run on the fixed head, and the new runs are added here.

### 3.2 Mutant re-runs on the fixed head `4c81ba3`

Mutants M1–M8 were merged forward onto `4c81ba3` (one `write` per line) and re-run. Each one is red on `smoke-accmode` (shard 8) on its designed arm:

| Mutant | Run | Printed |
|---|---|---|
| M1 | [40158002227](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158002227) | A `ro_write=4` (and F `dup_ro_write=4`) |
| M2 | [40158003600](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158003600) | B `wo_read=8` |
| M3 | [40158004901](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158004901) | C `ring_ro_write=4 ring_wo_read=4` |
| M4 | [40158006699](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158006699) | D `ro_ftrunc=0 size=0` |
| M5 | [40158008354](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158008354) | E `acc3=3 ro_trunc=3 size=0` |
| M6 | [40158009849](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158009849) | F `dup_ro_write=4` |
| M7 | [40158011102](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158011102) | G `wo_mmap=32767` |
| M8 | [40158012624](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158012624) | H `rw_write=-9 rw_read=4` |

No arm line was torn in any run.

Side failures, outside `smoke-accmode`:
- **M8, shard 4:** `smoke-ftruncate` (`FTRUNC FAIL: initial write short`). This is expected: M8 refuses writes on `O_RDWR`.
- **M2, shard 0:** `smoke-shm` arm C (`PRADYOS_SHM_C cycles=256 leaked_ok=1` not found). M2 changes `read` only, and the head `4c81ba3` passed shard 0. This is recorded as a possible intermittent failure of `smoke-shm` arm C on the base tree; it is **not root-caused**.

## 4. Honesty labels

- Gated by `smoke-accmode` arms A-H.
- Emulated (QEMU TCG) only, not verified on real hardware.
- Closes the DDR-1171 §7 access-mode gap for `FD_VFS` fds. Console direction stays open, as stated in §2.
- Does not touch U1-U3 or OPEN-1/2/12/13.
