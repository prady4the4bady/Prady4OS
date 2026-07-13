# DDR-745 — PRISM `touch` / `rm`: a writable shell on the real FAT root

**Status:** proposed (pre-code)
**Layer:** user (PRISM) only. Consumes DDR-744's syscalls; no kernel change.

## Problem

DDR-744 gave ring 3 `O_CREAT`-on-open + `SYS_UNLINK`, but the only consumer is a
freestanding SFS-rooted probe. PRISM — the interactive shell — is rooted at the
**FAT** default mount, and the FAT32 driver already implements `create`,
`write`, and `unlink` (`kernel/fs/fat32/fat32.c`). So the syscalls already work
on PRISM's real root; nothing wires them into the shell. `touch`/`rm` are the
natural missing builtins.

## Decision

Two PRISM builtins, both one-liners over the existing NSIs (no kernel change):

- **`touch <path>`** — `SYS_OPEN(path, O_CREAT|O_WRONLY)`; on success `close` and
  print `touch: <path>` (an empty file, or a no-op open if it already exists —
  create-if-absent, matching DDR-744). Error → `touch: cannot create <path>`.
- **`rm <path>`** — `SYS_UNLINK(path)`; success → `rm: removed <path>`, else
  `rm: cannot remove <path>`.

Both resolve against PRISM's root mount (FAT), so they exercise the DDR-744
syscall path through the real shell on the default root — not a test probe. The
`help` line and the `docs` builtin list gain `touch`/`rm`.

## Gate — extend `smoke-shell` (no new gate; stays 84)

The FAT data image (`build/fat.img`) is rebuilt fresh per gate run, so writing to
it during the test is ephemeral. Feed, in order:
`touch /PRISMNEW.TXT` → `ls /` → `rm /PRISMNEW.TXT`. Assert:
- `(^|prism> )PRISMNEW\.TXT$` appears — the `ls` after `touch` lists the created
  file (proves `O_CREAT` through the shell on the FAT root), using the same
  prompt-tolerant anchor as the DDR-742/743 `ls` assert.
- `rm: removed /PRISMNEW.TXT` appears — proves `SYS_UNLINK` succeeded.

(A "gone after rm" assertion is skipped: the earlier `ls` line for the file
stays in the serial log, so a negative grep would be unreliable. The `rm:
removed` confirmation is the deterministic witness.)

## Non-goals

- No `mkdir` (FAT32 `create` makes only regular files; explicit empty-dir
  creation is a separate op — a later slice if needed).
- No `-r`/recursive, no globbing, no multi-arg `rm`; one path per invocation.
- No overwrite/redirect (`>`); `touch` makes an empty file only.
