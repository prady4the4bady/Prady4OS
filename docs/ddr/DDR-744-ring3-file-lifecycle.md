# DDR-744 — ring-3 file lifecycle: `O_CREAT` on open + `SYS_UNLINK`

**Status:** proposed (pre-code)
**Layer:** syscall + user probe. Closes the ring-3 VFS write gap.

## Problem

Ring 3 can `open`/`read`/`readdir` (DDR-742) and, since DDR-741, the SFS backend
can create hierarchical dirs and unlink files/empty dirs — but *only the kernel*
can reach those mutations. `sys_open` ignores its `flags` for creation (no
`O_CREAT`), and there is no `SYS_UNLINK`. So a ring-3 program cannot create or
delete a file at all; the DDR-741 unlink path is proven only by an in-kernel
self-test (`[sfs] unlink/rmdir OK`), never across the syscall boundary.

## Decision

Two additions, both resolved against the caller's `root_mnt` + `fs_cap`
(honoring per-process roots, DDR-739), exactly like `sys_open`/`sys_getdents`:

1. **`O_CREAT` (0x40) on `sys_open`.** If `vfs_open` fails and `O_CREAT` is set,
   fall back to `vfs_create` (which needs `CAP_FS_WRITE` — every ELF-loaded proc
   already holds `CAP_FS_READ | CAP_FS_WRITE`, elf.c). On success the fd is wired
   the same way. No `O_EXCL`/mode bits yet (single-writer create; a later slice).
   This matches Linux's `O_CREAT` numeric value (octal 0100).

2. **`SYS_UNLINK` (NSI 68)** — `(path) -> 0 | -errno`. `copyinstr` the path,
   then `vfs_unlink(fs_cap, root_mnt, path)`. Removes a file or an empty dir
   (SFS tombstones, DDR-741). The SFS backend returns only `0`/`-1` (it does not
   distinguish absent-vs-non-empty-vs-tombstone), so the syscall collapses any
   non-zero backend rc to `-ENOENT` — a non-empty dir therefore also reports
   `-ENOENT`, not a distinct `-ENOTEMPTY`. A richer backend errno is a later
   slice (it would require SFS's unlink to return real `-E*` codes first).

3. **`FD_VFS` write in `sys_write`.** Discovered while building the probe: the
   file-write path was a stub (`FD_VFS -> -EBADF`, "arrives in slice 4", never
   landed), so a created file could not be populated from ring 3 — only the
   kernel's direct `vfs_write` worked. Implemented it mirroring the `FD_VFS` read
   path: chunked `copyin` into a bounded kernel buffer, `vfs_write(e->cap,
   e->file, e->off, …)` (enforces `CAP_FS_WRITE` + the per-process write budget),
   advancing the fd offset by the bytes actually written; a backend short-write
   returns the partial. Without this, create + unlink is untestable end-to-end.

FAT32 (the default root) has no `create`/`unlink` op wired for ring 3 here, so
the probe runs against an **SFS root** (the writable CoW volume), spawned the
same hand-rolled way as the DDR-739 ext4 probe: `elf_load` the embedded bytes,
set `root_mnt = smnt` before `sched_unblock`.

## Gate — `smoke-fsrm` (new; 83 → 84)

A freestanding probe `user/fsrmtest.c` (musl-free, `user.ld`) runs the full
lifecycle against its SFS root and prints sentinels via raw `SYS_WRITE`:
- `open("/RMPROBE", O_CREAT|O_WRONLY)` → fd ≥ 0, `write` bytes, `close`.
- `open("/RMPROBE", O_RDONLY)` → fd ≥ 0 (create landed), read back, `close`.
- `unlink("/RMPROBE")` → 0.
- `open("/RMPROBE", O_RDONLY)` → < 0 (gone).
- `unlink("/RMPROBE")` again → < 0 (already gone, idempotent-safe).
On all-pass it prints `PRADYOS_FSRM_OK`; any step wrong prints `PRADYOS_FSRM_FAIL`.
`smoke-fsrm` asserts `PRADYOS_FSRM_OK` (EXTRA_SENTINEL) with `FSRM FAIL`
forbidden, via `boot_test.sh`.

Regression: `smoke-fsrm`, `smoke-fs-sfs-rw` (shares the SFS volume), `smoke-shell`
(FAT root unaffected — create/unlink there still return the backend's error),
then the full suite.

## Non-goals

- No `O_EXCL`, `O_TRUNC`, `O_APPEND`, or mode/permission bits — create-if-absent
  only.
- No FAT32 create/unlink for ring 3 (FAT stays read-mostly at the syscall edge).
- No `rename`; no recursive `rm -r`. `SYS_UNLINK` is one name at a time.
- PRISM `rm`/`touch` builtins deferred — PRISM's root is FAT, so wiring shell
  builtins needs the FAT write path or a PRISM-on-SFS decision (later slice).
