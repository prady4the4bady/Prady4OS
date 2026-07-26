# DDR-781 — PRISM input redirection `<` and append `>>`

**Status:** implemented — locally proven, all discriminating checks PASS: append
kept **both** records (`aaa-8w1` + `bbb-8w1`), `<` delivered stdin from the file
(marker present, no `cat: cannot open <`), the DDR-780 pipe still works, shell
alive, zero panics. Third bounded slice of master-doc **Section B, item 12**;
follows DDR-778 (`>`) and DDR-780 (`|`).

## Prerequisite findings (checked in the tree, and one changed the plan)

- **The kernel has NO `O_APPEND`.** `kernel/syscall/sys_file.c` defines and
  honours only `O_CREAT 0x40`; there is no `O_APPEND` and no `O_TRUNC`. The
  originally-planned `O_CREAT|O_WRONLY|O_APPEND` for `>>` **is not available**, and
  adding a kernel open-flag would be a silent scope expansion.
- **But `SYS_LSEEK = 10` exists and supports `SEEK_END`**
  (`sys_proc.c`: `case SEEK_END: base = e->file->size`). So append is achievable
  entirely in ring 3: `open(O_CREAT|O_WRONLY)` then `lseek(fd, 0, SEEK_END)`.
- `O_RDONLY` is flag `0` (already used by `do_cat`), so `<` needs nothing new.

**Result: still ring-3 only, no kernel change** — the prerequisite check changed
the *mechanism* (lseek instead of an open flag) rather than the scope.

### Honest limitation of lseek-based append

`lseek(SEEK_END)`-at-open is **not** atomic `O_APPEND`: with concurrent writers to
the same file, writes could interleave or overwrite. That is acceptable here
because a PRISM redirect has exactly one writer for the duration of one command.
Genuine atomic append would need kernel `O_APPEND`, and that is recorded as a
non-goal rather than pretended.

### Pre-existing limitation this exposes (not introduced here)

There is **no `O_TRUNC` either**, so DDR-778's `>` does not truncate: rewriting a
long file with shorter content leaves trailing stale bytes. Out of scope for this
slice, but recorded, because it is surprising and will eventually need kernel
support.

## Decision — `user/prism.c` only

Extend the DDR-778 redirect scan to recognise three tokens — `<`, `>`, `>>` —
truncating `argc` at the **first** one so the builtin sees only its own arguments:

- `<  file` → `open(file, O_RDONLY)`, save fd 0 via `dup2(0, REDIR_SAVE_IN)`,
  `dup2(fd, 0)`.
- `>  file` → as DDR-778 (`O_CREAT|O_WRONLY`), save fd 1.
- `>> file` → same open, then `lseek(fd, 0, SEEK_END)` before `dup2(fd, 1)`.

Both saves are restored at the loop's existing bottom-of-loop flush, in the same
place and manner DDR-778 established, so the "restore sits on the single path
every dispatched command ends on" invariant is preserved. A redirect token with no
following filename is a clean error and the command does not run (**S2**).

## Gate — extend `smoke-shell`, discriminatingly

Both assertions are chosen so a broken implementation cannot pass:

1. **Append:** `echo aaa-8w1 > /APP.TXT` then `echo bbb-8w1 >> /APP.TXT` then
   `cat /APP.TXT` — **both** markers must appear. If `>>` silently behaved like
   `>`, the second write would start at offset 0 and overwrite the first record
   (same length), so `aaa-8w1` would be **gone** — the pair is what discriminates.
   Note this works *because* there is no `O_TRUNC`; the test asserts observed
   behaviour, not assumed behaviour.
2. **Input:** `echo in-marker-8w1 > /IN.TXT` then `cat < /IN.TXT` — the marker
   must appear **and** `cat: cannot open <` must **not**. If `<` were ignored,
   `cat` would receive `<` as its path argument and print exactly that error.

## Architecture prerequisite checklist

- New NSI/syscalls: **none** — `SYS_LSEEK`/`SYS_OPEN`/`SYS_DUP2` all already ship.
  NSI range stays at 75. No kernel change.
- TCB/roster fields, PMM/VMM, capability gates, AETHER queue/audit, scheduler
  hooks, network policy, compositor/UI: **none**.
- Filesystem/root-mount: uses the existing FAT root the shell already writes to.
- New gate: none — `smoke-shell` extended.
- **Security invariants:** **S2** — parsing stays within the existing
  `argv[16]`/`line[256]` bounds; malformed redirects fail cleanly instead of
  running; no new loops. **S6** — a failed `open`/`lseek` is handled in ring 3 and
  cannot affect the kernel; fd juggling is confined to this process's table.
  S1/S3–S5/S7/S8 not engaged; W^X, NX and capability contracts untouched.

## Non-goals

Atomic `O_APPEND` and `O_TRUNC` (both need kernel support — separate slice),
stderr redirection (`2>`), multi-stage pipelines, and job control.
