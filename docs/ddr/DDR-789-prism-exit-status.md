# DDR-789 — PRISM exit status `$?`

**Status:** implemented — locally verified: `st-ok=0`, `st-fail=127` after a failed
`run`, no literal `$?` in the log, pipeline status 0, and the DDR-786 3-stage /
DDR-787 big-pipe (200/200) regressions intact. Zero panics, zero build warnings. Sixth shell slice of master-doc
**Section B, item 12**; follows DDR-784 (`2>`), DDR-786 (`a|b|c`) and DDR-787
(blocking pipes).

## Why this before SIGPIPE — a prerequisite finding that reordered the queue

SIGPIPE was the queued candidate (DDR-787 returns `-EPIPE` with no signal). The
tree check found two things:

1. **Signal defaults are a whitelist, not a table.** `signal_deliver`
   (`kernel/proc/signal.c`) terminates on `SIGKILL`, terminates on `SIGTERM` when
   no handler is installed, and **ignores everything else** with no handler. So
   defining `SIGPIPE 13` and raising it would silently do *nothing* — the default
   list would have to be extended too. Small, but not the one-liner it looks like.
2. **SIGPIPE cannot be gated discriminatingly today.** Proving it works needs a
   reader that consumes part of its input and exits (a `head`), which PRISM does
   not have; and the difference between "writer killed promptly" and "writer ran
   to completion writing into a dead pipe" is invisible in the serial log, because
   a stage's outcome is not observable at all. A gate that cannot fail on the old
   behaviour is not a gate.

Exit status is what makes outcomes observable, and a signal-killed thread exits
`-1` (`sched_exit(-1)`), so `$?` is *also* the mechanism that will let a future
SIGPIPE slice discriminate. Hence this first.

## Prerequisite findings for `$?` itself

- **`sys_wait4` already yields what is needed:** it copies out
  `child->exit_status` — the **raw** exit code, explicitly not POSIX `W*`-encoded
  (`kernel/syscall/sys_wait.c`). No kernel change, no encoding to decide.
- **PRISM already collects it and throws it away:** `do_run` does
  `nsi(SYS_WAIT4, kid, (long)&st, 0)` and DDR-786's pipeline parent reaps each
  stage into `st`. The plumbing is half-built; this slice keeps the value.

**Ring-3 only, no kernel change** — confirmed before designing.

## Decision — `user/prism.c` only

- Keep `static long last_status`, updated wherever a child is reaped: `do_run`,
  and DDR-786's per-stage loop (the **last** stage's status wins, which is what a
  shell reports for a pipeline).
- Expand a token **ending in** `$?` to `<prefix><decimal status>`, after
  splitting. Deliberately **not** general variable expansion: no `$VAR`, no
  substitution mid-token, no quoting rules — those are a separate slice, and
  pretending otherwise would be scope creep.

  **This was "a token exactly equal to `$?`" in the first draft, and testing
  proved that useless.** The natural idiom is `echo status=$?`, whose token is
  `status=$?`, so the exact-match rule expanded nothing and the gate printed the
  literal. Rather than contort the test to fit the design (`echo status= $?`,
  which prints a stray space), the design widened to the suffix rule — still far
  short of general expansion, and it covers the idiom people actually write.
- Builtins that fail set `last_status` to 1 so `$?` means something for them too;
  a builtin that succeeds sets 0.

## Gate — extend `smoke-shell`, discriminatingly

```
echo status=$?
run /NOPE789.ELF
echo status=$?
```

Assert:

1. `status=0` **present** — a successful command reports 0.
2. `status=$?` **absent** — this is the discriminator. Before this slice the
   tokenizer passes `$?` through untouched and `echo` prints it *literally*, so
   the old behaviour produces exactly that string and fails the gate
   deterministically.

3. `status=127` **present** after the failed `run`. This turned out to be
   deterministic after all: `do_run`'s child already does
   `nsi(SYS_EXIT, 127, 0, 0)` when `execve` fails, which is the conventional
   shell "command not found" code. So the gate can assert the *value*, not merely
   that expansion happened — a stronger check than the draft assumed.

## Architecture prerequisite checklist

- **New syscalls / NSI:** none — `SYS_WAIT4` already returns the status. NSI stays
  at 75. **No kernel change.**
- TCB/roster, PMM/VMM, capability gates, AETHER, scheduler hooks, network policy,
  compositor, FS/on-disk format: **none**.
- **New gate:** none — `smoke-shell` extended.
- **Security invariants:** **S2** — expansion is a single fixed-length
  substitution into the existing `argv[16]`/`line[256]` bounds, with no new loop
  and no allocation. **S6** — ring-3 only; a wrong status is a display bug, not a
  fault path. S1/S3–S5/S7/S8 not engaged. No invariant weakened.

## Non-goals

General variable expansion (`$VAR`, `${...}`, quoting), POSIX `W*` status
encoding, `set -e`, `$!`/`$$`, and SIGPIPE itself — which this slice unblocks
rather than implements.
