# DDR-780 — PRISM pipes (`cmd1 | cmd2`)

**Status:** implemented — locally proven (CI blocked by the DDR-779 musl outage).
Discriminator PASS: the serial shows the bare marker `pipe-marker-4k8` with **no
trailing `| cat`**, i.e. it crossed the pipe rather than being echoed literally;
the shell stays alive afterwards; no panic. DDR-778 redirection and the existing
builtins re-verified unaffected. Second bounded slice of master-doc **Section B,
item 12**; follows DDR-778 (output redirection).

## Problem

DDR-778 added `>`. The remaining high-value piece is `|`. The obstacle is
structural, and was identified in DDR-778 rather than rediscovered here:
**PRISM builtins are internal functions inside one big `if/else` chain in
`main()`, not separate executables.** So `|` cannot be implemented by
`fork`+`exec` the way a normal shell would — the fork has to wrap the *builtin
dispatch itself*.

## Prerequisite check (the recurring lesson)

Verified in the tree, not assumed:

- `SYS_PIPE` and `SYS_DUP2 (=18)` exist in the kernel (PROC-A), gated by
  `smoke-syspipe` and CI's "pipe/dup2" step. `SYS_DUP2` is already `#define`d in
  `prism.c` (DDR-778).
- `SYS_FORK` / `SYS_WAIT4` are already used by the `run` builtin, so the fork/reap
  pattern exists in this file.
- **So this slice needs no kernel change** — ring-3 only, like DDR-778.

## Decision

1. **Extract the dispatch.** Move the `if/else` builtin chain out of `main()` into
   `static int run_builtin(int argc, char **argv)`. Return value: `0` = continue,
   `1` = the shell should exit (the `exit` builtin, which currently does
   `return 0` straight out of `main`). This is a mechanical extraction — no
   behaviour change — and it is what makes a builtin runnable inside a forked
   child.
2. **Parse one `|`.** Split `argv` at the first bare `|` into a left and right
   command (exactly one pipe this slice; `a | b | c` is rejected cleanly).
3. **Run it:**
   - `pipe(fds)` → `fds[0]` read end, `fds[1]` write end;
   - fork **left**: `dup2(fds[1], 1)`, close **both** ends, `run_builtin(left)`,
     `_exit`;
   - fork **right**: `dup2(fds[0], 0)`, close **both** ends, `run_builtin(right)`,
     `_exit`;
   - **parent: close BOTH ends immediately**, then `wait4` twice.
4. **fd hygiene is the whole risk.** If the parent keeps the write end open, the
   right-hand command never sees EOF and the shell wedges; if a child keeps the
   opposite end open, likewise. Every one of the three processes closes both
   original descriptors after duplicating what it needs. The parent reaps **both**
   children so no zombie is left for init (**S2** — bounded: exactly two children,
   two waits).
5. **`cat` with no argument now reads stdin.** Discovered while designing the
   gate: PRISM's `cat` took a *path* and printed usage with no args, and **no
   builtin consumed fd 0 at all** — so a pipe would have been unobservable and
   useless. `cat` with no argument now copies stdin → stdout (standard shell
   behaviour, bounded 256-byte reads, EOF-terminated).
6. **Interaction with `>` (DDR-778).** Redirection is parsed *after* the pipe
   split, so `a | b > f` redirects only the right-hand side, which matches shell
   semantics. The parent's stdout is never swapped in the pipe path, so DDR-778's
   restore invariant is untouched.

## Gate — extend `smoke-shell`, discriminatingly

`echo pipe-marker-4k8 | cat` must print the marker — and the assertion is a
**pair**, because presence alone proves nothing:

1. `pipe-marker-4k8` must appear (the marker crossed the pipe into `cat`'s fd 0);
2. `pipe-marker-4k8 | cat` must **never** appear.

(2) is what makes it discriminating. If `|` were ignored, `tokenize()` splits only
on spaces, so `echo` would receive the literal tokens `pipe-marker-4k8 | cat` and
print them verbatim — the marker would still satisfy (1). Requiring the pipe
tokens to be *absent* from the output is therefore the real test.

**A rejected assertion, recorded so it is not re-proposed:** "`ls / | cat` must
produce `HELLO.TXT`" looks stronger but is **not** discriminating in this gate —
the FIFO script already runs a plain `ls /` earlier in the same session, so
`HELLO.TXT` appears whether or not the pipe works.

`cat` with no argument had to learn to read stdin for any of this to mean
anything (see Decision), since no PRISM builtin previously consumed fd 0.

## Architecture prerequisite checklist

- New NSI/syscalls: **none** (NSI stays at 75). No kernel change at all.
- TCB / roster fields, PMM/VMM mappings, capability gates, AETHER queue/audit,
  scheduler hooks, network policy, compositor/UI: **none**.
- Filesystem/root-mount: unchanged (uses the same FAT root).
- New gate: none — `smoke-shell` extended.
- **Security invariants:** **S2 (bounded everything)** — exactly one pipe, two
  children, two `wait4`s, parsing inside the existing `argv[16]`/`line[256]`
  bounds; `a|b|c` and a missing side fail cleanly rather than looping or
  spawning unboundedly. **S6 (fault isolation)** — a builtin now runs in a forked
  child, so a fault there kills only that child; the parent shell survives, and
  fd juggling stays inside each process's own table. S1/S3–S5/S7/S8 not engaged;
  W^X, NX and capability contracts untouched.

## Validation note (CI is blocked)

`git.musl-libc.org` is down (DDR-779), so CI cannot run. This slice is validated
**locally** with the DDR-778 FIFO proof pattern (serial log on `/tmp`, not
`build/` on DrvFs), extended to re-assert **all** existing `smoke-shell`
expectations plus redirection and pipes — i.e. equivalent coverage to the CI step,
run locally. `make smoke-shell` itself cannot pass on this machine for the
environmental reason recorded in DDR-778; its timeout was deliberately not raised.

## Non-goals

Multi-stage pipelines (`a | b | c`), `<`, `>>`, stderr redirection, and job
control (`&`, job table) — the last genuinely needs signal plumbing and is its own
slice.
