# DDR-784 — PRISM diagnostics on stderr, and `2>` redirection

**Status:** implemented — locally verified, every check PASS: the `2>`
discriminator (error reached fd 2 while fd 1 pointed at another file), stderr
still visible on the console when *not* redirected, the gate-asserted
`rm: removed …` still on stdout, and the DDR-780/782 pipe, truncate and append
regressions intact, zero panics, zero build warnings. Fourth shell slice of master-doc
**Section B, item 12**; follows DDR-778 (`>`), DDR-780 (`|`), DDR-781 (`<`, `>>`)
and DDR-782 (kernel `O_TRUNC`/`O_APPEND`).

## Prerequisite findings — one of them re-scoped the slice

- **PRISM has ZERO writers to fd 2.** `grep -c "fprintf(stderr|write(2|STDERR"
  user/prism.c` returns **0**; every diagnostic goes through `printf` → fd 1
  (`user/prism.c:130,148,184,189,246,254,269,309,321`). **So `2>` on its own would
  be untestable sugar** — it would redirect an fd nothing writes to, capture an
  empty file, and no discriminating gate could exist. The slice is therefore two
  honest halves, not one: **route diagnostics to stderr first**, then redirect it.
- **fd 2 already exists and already works.** `fd_init_std` (`kernel/proc/fd.c:23`)
  gives every process fds 0/1/2 as `FD_CONSOLE`, so `fprintf(stderr, …)` reaches
  the console today, and `sys_io.c`'s `fd_write_user` handles `FD_VFS` for *any*
  fd — so `dup2`-ing fd 2 onto a file needs nothing new. PRISM links musl
  (`Makefile:223`), and musl's stderr is unbuffered, so no new flush discipline is
  required. **Ring-3 only, no kernel change** (confirmed before designing, not
  assumed).
- **Found during implementation, not design: the musl SUBSET had no `stderr`.**
  The first link failed with `ld.lld: error: undefined symbol: stderr`.
  `tools/build_musl.sh` compiles a hand-listed subset (ADR-023 "minimal subset")
  that included `src/stdio/stdout.c` but **not** `stderr.c` or `fprintf.c` — and
  the obvious workaround (`snprintf` + a raw `SYS_WRITE` to fd 2) is *also*
  unavailable, since `snprintf` is not in the subset either. So some subset
  growth was unavoidable; the two upstream stdio sources are the smaller and more
  honest choice than a bespoke formatting helper, and they give real POSIX
  semantics. Recorded here rather than quietly folded in: **this slice does touch
  the musl build list** (2 lines, upstream sources, no overlay change, no ADR-023
  pin change). musl's stderr is unbuffered, so it pulls in no buffering machinery
  beyond the write path already present.
- **Gate-asserted prints were checked before touching any message.** Only two
  PRISM strings appear in gates: `cat: cannot open <` (a *forbidden* assertion —
  unaffected, since stderr still reaches the console) and **`rm: removed
  /PRISMNEW.TXT` (a *required* assertion)**. That second one is a success message
  and **must stay on stdout** — moving it would silently break `smoke-shell`.

## Decision — `user/prism.c` only

1. **Diagnostics → `fprintf(stderr, …)`.** Genuine error messages only: `cat:
   cannot open`, `run: fork failed`, `run: usage`, `pipe: missing command`,
   `pipe: unavailable`, `redirect: missing filename`, `redirect: cannot open`,
   `ls: … empty or not a directory`, `mode: denied`. **Informational and success
   messages stay on stdout** — `rm: removed …` (gate-asserted), `mode: set …`,
   `help`, and all command output. The split is "did the command fail", not
   "does the line start with a name".
2. **`2>` in the existing redirect scan**, symmetric with DDR-778/781: a third
   parking slot `REDIR_SAVE_ERR 11` (FD_MAX is 64), `open(O_CREAT|O_WRONLY|
   O_TRUNC)` — the same truncating flags DDR-782 gave `>` — then `dup2(fd, 2)`,
   restored at the loop's existing bottom-of-loop restore point alongside fds 1
   and 0, preserving the DDR-778 invariant that the restore sits on the single
   path every dispatched command ends on.

Swap order is **stdin → stderr → stdout**, and every failure path unwinds exactly
the swaps already made. Ordering stderr *before* stdout means a later
`redirect: cannot open` for the stdout file is itself written to the redirected
stderr — which is what a real shell does.

## Gate — extend `smoke-shell`, discriminatingly

The obvious test does **not** discriminate: `cat /NOPE 2> /ERR.TXT` then
`cat /ERR.TXT` asserts the error text is present, but a *broken* `2>` prints that
same text straight to the console, so the assertion passes either way.

The discriminator is to **redirect stdout and stderr in the same command, to
different files**:

```
cat /NOPE9k2.TXT > /OUT9k2.TXT 2> /ERR9k2.TXT
cat /ERR9k2.TXT
```

- **Working:** the error goes to fd 2 → `/ERR9k2.TXT`, and the second command
  prints `cat: cannot open /NOPE9k2.TXT` — **present** in the log.
- **Broken (today's behaviour):** the error goes to fd 1, which is redirected to
  `/OUT9k2.TXT`, so it never reaches the console and `/ERR9k2.TXT` is empty —
  **absent** from the log.

So the assertion fails before this change and passes after, and it simultaneously
proves the message travelled on fd 2 rather than fd 1 — because fd 1 was pointed
somewhere else in the very same command.

## Architecture prerequisite checklist

- **New syscalls / NSI:** none. `SYS_OPEN`/`SYS_DUP2`/`SYS_CLOSE` all ship; NSI
  stays at 75. **No kernel change** — fd 2 is already `FD_CONSOLE` and the
  `FD_VFS` write path is fd-agnostic.
- **Third-party build:** `tools/build_musl.sh` gains `src/stdio/stderr.c` and
  `src/stdio/fprintf.c` (see the prerequisite findings). Upstream sources at the
  ADR-023 pinned commit — no overlay change, no pin change, no ADR needed.
- **TCB/roster, PMM/VMM, capability gates, AETHER queue/audit, scheduler hooks,
  network policy, compositor/UI, on-disk format:** none.
- **Capabilities:** unchanged — CAP_FS_WRITE already governs the `open`/`write`
  behind a redirect, exactly as for `>`.
- **Filesystem/root-mount:** the existing FAT root the shell already writes to.
- **New gate:** none — `smoke-shell` extended.
- **Security invariants:** **S2** — parsing stays inside the existing
  `argv[16]`/`line[256]` bounds, adds no loop, and a redirect token with no
  filename still fails cleanly without running the command. **S6** — a failed
  `open` is handled in ring 3, every failure path unwinds only the swaps already
  made, and all fd juggling is confined to this process's table. S1/S3–S5/S7/S8
  not engaged; W^X, NX and the capability contract untouched. No invariant is
  weakened or bypassed, so no human sign-off is required.

## Non-goals

`2>&1` / fd-duplication syntax, appending stderr (`2>>`), multi-stage pipelines,
job control, and any change to what the kernel considers a console fd.
