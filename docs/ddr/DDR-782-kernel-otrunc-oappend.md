# DDR-782 — kernel `O_TRUNC` + atomic `O_APPEND`

**Status:** proposed → implementing. Kernel-side remainder of master-doc
**Section B, item 12** (shell redirection). Closes the two gaps DDR-781 recorded
as *real defects in shipped behaviour that ring 3 cannot fix*.

## Why (the defects, not new sugar)

1. **`>` does not truncate.** DDR-778's `>` opens `O_CREAT|O_WRONLY` and writes
   from offset 0. Rewriting a long file with shorter content leaves the tail of
   the old content in place, so `cat` shows a corrupt mixture. Every shell user
   expects `>` to replace, not overlay.
2. **`>>` is not atomic append.** DDR-781 implements it as a one-shot
   `lseek(fd, 0, SEEK_END)` at open time. Correct for one writer, wrong the
   moment two writers share a file — POSIX `O_APPEND` re-positions at
   end-of-file on *every* write, and only the kernel can do that.

Stacking more shell features (`2>`, `a|b|c`) on top of a `>` that silently
corrupts files is the wrong order of work.

## Prerequisite finding — and it re-scoped this slice

`struct vfs_fs_ops` (kernel/fs/vfs/vfs.h:35-49) exposes
`mount/open/create/read/write/unlink/readdir/txn_*/umount`. **There is no
`truncate` op, and none of FAT32, SFS or ext4 implements shortening a file.**

Adding a real `truncate` op would mean a new VFS op implemented three times, in
three on-disk formats — a multi-slice project, not this slice. Per the standing
rule, that is reported rather than silently invented.

**Re-scope — both flags are implementable with primitives that already ship:**

- **`O_TRUNC` = `vfs_unlink` + `vfs_create`** in `sys_open`, only when the file
  already exists. Truncation to zero is exactly what a shell `>` needs, and this
  works identically on all three drivers with no FS change.
  **Honest limitation:** the file is *re-created*, not shortened in place, so it
  gets a fresh inode/cookie. With no hard links and no shared-fd-across-open
  semantics in the tree, nothing observable depends on inode identity today.
  Non-zero `ftruncate(len)` remains impossible and stays a non-goal.
- **`O_APPEND` = fd-layer only.** `sys_io.c`'s FD_VFS write path already keys off
  `e->off`; setting `e->off = e->file->size` at the **entry of each `sys_write`
  call**, before the chunk loop, is precisely the POSIX atomicity property (each
  `write()` lands at end-of-file as one act). Doing it per *chunk* instead would
  be no stronger and would obscure the contract. No FS change at all.

`struct fd_entry` already stores `e->flags` from open (sys_file.c), so neither
change needs a new field.

## Decision

- `kernel/syscall/sys_file.c`: `#define O_TRUNC 0x200` and `#define O_APPEND 0x400`
  (Linux values, matching the existing `O_CREAT 0x40`). In `sys_open`, after a
  successful *existing-file* open with `O_TRUNC` set, `vfs_unlink` + `vfs_create`
  the path; a failure of either is `-EIO` and the fd is not handed out.
  `O_TRUNC` needs CAP_FS_WRITE, which `vfs_unlink`/`vfs_create` already enforce —
  **no capability change** (CAP_FS_WRITE already governs all writes).
- `kernel/syscall/sys_io.c`: in the FD_VFS write branch, before the chunk loop,
  `if (e->flags & O_APPEND) e->off = e->file->size;`.
- `user/prism.c`: `>` gains `O_TRUNC`, `>>` gains `O_APPEND` and drops the
  `lseek(SEEK_END)` crutch.

## Gate — extend `smoke-shell`, discriminatingly

`echo <long-marker> > /TR.TXT` then `echo <short-marker> > /TR.TXT` then
`cat /TR.TXT`. Assert the short marker **present** AND a trailing fragment of the
long marker **absent**. Today's non-truncating `>` leaves that fragment, so the
assertion **fails before this change and passes after** — it discriminates.
Append keeps DDR-781's both-records-survive assertion, which now exercises
`O_APPEND` instead of `lseek`.

## Architecture prerequisite checklist

- **New syscalls / NSI:** none. Two open *flags* on the existing `SYS_OPEN`;
  NSI range stays at 75.
- **TCB/roster fields, PMM/VMM, AETHER queue/audit, scheduler hooks, network
  policy, compositor/UI:** none.
- **fd/VFS:** no new `vfs_fs_ops` member, no FS driver change, no on-disk format
  change. `fd_entry` unchanged (`flags` already stored).
- **Capabilities:** unchanged — CAP_FS_WRITE already gates `vfs_create`,
  `vfs_unlink` and `vfs_write`; `O_TRUNC` cannot escalate a read-only opener.
- **Filesystem/root-mount:** existing per-process root mount.
- **New gate:** none — `smoke-shell` extended.
- **Security invariants:** **S2 (bounded)** — both paths are O(1) additions with
  no new loop; `O_TRUNC` failure aborts cleanly instead of leaving a half-open
  fd; `O_APPEND` writes stay inside the existing chunk-loop bounds.
  **S6 (fault isolation)** — a failed unlink/create returns an errno to the
  caller and cannot damage kernel state; the offset override touches only the
  calling process's fd table. S1/S3–S5/S7/S8 not engaged; W^X, NX and the
  capability contract are untouched. **No invariant is weakened or bypassed**, so
  no human sign-off is required.

## Non-goals

Non-zero `ftruncate(len)`, a real VFS truncate op, in-place shortening,
`O_EXCL`, stderr redirection, multi-stage pipelines, job control.
