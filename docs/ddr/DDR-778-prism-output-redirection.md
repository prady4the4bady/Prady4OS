# DDR-778 — PRISM output redirection (`cmd … > file`)

**Status:** proposed (pre-code). First bounded slice of master-doc **Section B,
item 12** ("Pipes/redirection/job control in PRISM").

## Problem

PRISM has no redirection or pipes. Section B#12 bundles three things —
redirection, `|`, and job control — which is too much for one slice. This DDR
takes only **output redirection for a single command**, the smallest piece that is
independently useful and independently gateable.

## Verified before scoping (the B#5/B#7 lesson)

Section B entries have repeatedly turned out to be already-built, so the state was
checked against the tree first:

- **The kernel side already ships.** `SYS_DUP2 = 18` exists
  (`kernel/syscall/syscall.h`, registered by `pipe_register()`, PROC-A), as does
  `SYS_PIPE`, with gates `smoke-syspipe` and the CI "pipe/dup2" step.
  **`prism.c` simply never `#define`d `SYS_DUP2`.**
- `FD_MAX = 64` (`kernel/proc/fd.h`), so a high save-slot fd is available.
- `O_CREAT | O_WRONLY` are already used by the `touch` builtin.

**So this slice needs no kernel change at all** — it is strictly ring-3.

## Decision — `user/prism.c` only

1. `#define SYS_DUP2 18` and a `REDIR_SAVE_FD` slot.
2. After tokenising, scan `argv[1..argc)` for a bare `>` token. On match, record
   the following token as the target and **truncate `argc` at the `>`** so the
   builtin sees only its own arguments. Scanning starts at index 1 so a leading
   `> file` cannot leave `argv[0]` undefined. A `>` with no following token is a
   clean error (`redirect: missing filename`) and the command does not run.
3. Redirect by fd juggling, using only existing syscalls:
   `dup2(1, REDIR_SAVE_FD)` to stash the real stdout, `open(target,
   O_CREAT|O_WRONLY)`, `dup2(fd, 1)`, `close(fd)`.
4. Restore at the **existing `fflush(stdout)` at the bottom of the loop**:
   `fflush(stdout)` → `dup2(REDIR_SAVE_FD, 1)` → `close(REDIR_SAVE_FD)`.

### Two hazards handled deliberately

- **musl buffers stdout.** When stdout is a file rather than the console it is
  fully buffered, so the builtin's output must be **flushed before** fd 1 is
  restored — otherwise it would be written to the *console* after the swap, i.e.
  the redirect would silently half-work. There is also a flush *before* the swap
  so nothing already buffered (e.g. the prompt) lands in the file.
- **A skipped restore would be catastrophic**, silently sending all later shell
  output into the file. Control flow was audited: the loop's only `continue` is
  the `argc == 0` case, which runs *before* any redirect is set up; the `break`
  inside `ls` is an inner loop; and `exit` does `return 0`, ending the process, so
  no restore is needed. The restore therefore sits on the single path that ends
  every dispatched command. The open-failure path `continue`s *before* fd 1 has
  been touched, so it cannot leak the redirect either.

## Gate — extend `smoke-shell`

Feed two commands over the existing FIFO and assert a **pair**, because either
assertion alone is falsifiable:

```
echo redir-ok-7q2 > /REDIR.TXT
cat /REDIR.TXT
ls /
```

- `REDIR.TXT` must appear in `ls /` — proves the file was actually created;
- `redir-ok-7q2` must appear — from `cat`.

Asserting only the marker would **pass even if redirection did nothing**, since a
non-redirecting `echo` prints the same string to the console. Requiring the file
to exist is what makes the test discriminating. Deterministic: fixed marker, fixed
filename, no timing dependence beyond the FIFO pacing the gate already uses.

Existing `smoke-shell` sentinels are untouched (no prompt or builtin output
changes); `boot_test`-style `grep -qF` substring matching is not even involved
here — `smoke-shell` greps its own serial log directly.

## Architecture prerequisite checklist

- New NSI/syscalls: **none** — `SYS_DUP2` already exists; only a `#define` is
  added in userspace. NSI range stays at 75.
- TCB / roster / agent-slot fields: none. PMM/VMM: none. Capability gates: none
  (no new authority — the shell already opens files via `touch`).
- AETHER queue/audit, scheduler hooks, network policy, compositor/UI: none.
- Filesystem/root-mount: uses the existing FAT root the shell already writes to.
- New gate: none — `smoke-shell` is extended.
- **Security invariants:** **S2 (bounded everything)** — parsing stays inside the
  existing fixed `argv[16]` / `line[256]` bounds and adds no unbounded loop; the
  malformed-redirect case fails cleanly rather than running the command. **S6
  (fault isolation)** — a failed `open` is handled in ring 3 and cannot affect the
  kernel; the fd juggling is confined to this process's fd table. S1/S3–S5/S7/S8
  are not engaged: no agent, capability, audit, or objective-function surface.
  W^X, NX and capability contracts untouched.

## Non-goals (future B#12 slices)

- `|` between commands (needs fork around the **builtin dispatch**, since PRISM
  builtins are internal functions rather than execs — recorded here so the next
  slice starts from that fact rather than rediscovering it).
- Input redirection `<`, append `>>`, stderr redirection.
- Job control (`&`, background job table, `jobs`/`fg`) — that genuinely needs
  signal plumbing and is a separate slice.
