# DDR-742 — SYS_GETDENTS: ring-3 directory listing (`ls` works)

**Status:** proposed (pre-code)
**Layer:** syscall + user (PRISM). Closes a documented 5e follow-up.

## Problem

The VFS has `vfs_readdir` (used by the kernel FS self-tests and now spanning the
SFS hierarchy, DDR-738), but ring 3 has no way to enumerate a directory — there
is no getdents syscall. PRISM's `ls` is a stub (`"ls: directory listing pending
SYS_GETDENTS (5e+)"`). This is the last piece for a usable shell over the
existing FS.

## Decision

**`SYS_GETDENTS` (NSI 66)** — `(path, index, name_buf) -> len | 0 | -errno`,
per-entry to match `vfs_readdir`'s index API exactly (no in-kernel buffer
packing, no new struct ABI). The handler mirrors `sys_open`: resolve `path`
against the caller's `root_mnt` with `fs_cap` (so it honors per-process roots,
DDR-739), call `vfs_readdir(fs_cap, root_mnt, path, index, name, &sz)`, and
`copyout` the NUL-terminated name. Returns the name length (`> 0`), `0` when
`index` is past the last entry (clean end-of-directory), or `-EFAULT`/`-ENOENT`.
Bounded: name ≤ 255 + NUL staged in the kernel; the user buffer is written only
via `copyout`.

**PRISM `ls`** — `ls [dir]` (default `/`) loops `SYS_GETDENTS(dir, i, name)` from
`i = 0` until it returns `<= 0`, printing each name; prints an empty note if the
first call returns 0. It resolves against PRISM's own root (FAT), so `ls /`
lists the boot-volume entries.

## Gate — extend `smoke-shell` (no new gate; stays 83)

`smoke-shell` already drives PRISM over a FIFO (echo/help/exit). Add an `ls /`
command before `exit` and assert the serial shows `HELLO.TXT` (a known FAT-root
entry). This proves the syscall end-to-end through the real shell. The existing
echo/help/prompt/no-panic assertions are unchanged.

Regression: `smoke-shell`, `smoke-sysfile` (fd/open path shares the resolver),
`smoke-fs`, then the full suite.

## Non-goals

- No POSIX `struct dirent` / packed-buffer getdents(2) ABI — the per-entry form
  is what PRISM needs; a batched form is a later refinement if a libc wants it.
- No entry types/inode numbers/sizes in the result (name only) — `readdir` today
  yields names; `stat`-style metadata is a separate slice.
- `ps` stays a stub (needs a process-table syscall, not this).
