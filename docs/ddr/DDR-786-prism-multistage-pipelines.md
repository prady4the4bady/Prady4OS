# DDR-786 — PRISM multi-stage pipelines `a | b | c`

**Status:** implemented — locally verified, all ten checks PASS: the 3-stage
discriminator (marker crossed two pipes AND no stray `|` argument), a 4-stage
pipeline, malformed `a | | cat` rejected cleanly, the DDR-780 single-pipe,
DDR-784 `2>`/stderr and DDR-782 truncate/append regressions, and the shell still
alive afterwards (no wedge — the EOF hazard). Zero panics, zero build warnings. Fifth shell slice of master-doc
**Section B, item 12**; follows DDR-778 (`>`), DDR-780 (single `|`), DDR-781
(`<`, `>>`), DDR-782 (kernel `O_TRUNC`/`O_APPEND`) and DDR-784 (stderr, `2>`).

## Prerequisite findings

- **DDR-780's fall-through does NOT generalise as written.** Its pipe block sits
  *above* the dispatch and finds only the **first** `|`. The right-hand child sets
  `argv`/`argc` to the remainder and then falls through — it has already passed
  the pipe scan, so a second `|` reaches the builtin as a literal token. Today
  `echo m | cat | cat` gives `cat` the arguments `| cat`, and it tries to open a
  file named `|`. So this is a real gap, not cosmetic.
- **But N stages do NOT force the ~120-line refactor DDR-780 avoided.** The
  existing fall-through dispatch is reusable as-is; only the pipe block changes,
  by roughly 40 lines. Reported explicitly because the standing question was
  whether the deferred refactor had finally come due — it has not.
- **Kernel side is ready.** `pipe_create` heap-allocates per pipe with no fixed
  table (`kernel/proc/pipe.c`), so N-1 concurrent pipes are fine; `SYS_PIPE`,
  `SYS_FORK`, `SYS_DUP2`, `SYS_WAIT4` all ship. `argv[16]` bounds a pipeline at
  ≤ 8 stages. **No kernel change.**

### Pre-existing limitation this inherits (not introduced here)

`PIPE_SIZE` is 4096 and `pipe_write` is **non-blocking** — it returns short when
the ring is full and `fd_write_user` stops there. So a stage that produces more
than 4 KiB faster than its reader drains **loses data** rather than blocking.
That is already true of DDR-780's single pipe; multi-stage does not worsen it per
pipe. Recorded because it is surprising, and it bounds what pipelines can be used
for until pipe writes block (separate slice, kernel-side).

## Decision — two candidate designs, and why the more invasive one wins

**Design A (minimal, ~10 lines):** wrap the existing block in a loop so the *right*
child re-scans its remainder and forks again — recursion by iteration.
**Rejected.** It leaves an intermediate shell process at every stage boundary,
and that intermediate keeps the previous pipe's read end open (as its own fd 0)
for as long as it waits. With non-blocking 4 KiB pipes that does not hang, it
**silently drops output** when a downstream stage exits early — a data-loss
failure mode that is invisible in a small test and would be blamed on the kernel.

**Design B (chosen, ~40 lines):** the shell splits the line into N stages up
front, then forks each stage itself, threading one pipe between neighbours:

- keep `prev_read`; for stage *s* create a pipe unless it is the last;
- the child wires `prev_read → fd 0` (if any) and `fds[1] → fd 1` (if any),
  closes every pipe fd it holds, sets `argv`/`argc` to its own stage, marks
  `pipe_child` and breaks out to the existing dispatch;
- the parent closes `prev_read` and `fds[1]` immediately after each fork — **the
  parent must hold no pipe end open, or the reader never sees EOF and the shell
  wedges** (the DDR-780 lesson, now applied N times) — then keeps `fds[0]` as the
  next `prev_read`;
- after the loop the parent reaps all N children and `continue`s.

This yields the correct topology: N processes, one waiter, every pipe end closed
in exactly the processes that must not hold it. Bounded by `argv[16]` (**S2**);
a fault in any stage kills only that child (**S6**).

## Gate — extend `smoke-shell`, discriminatingly

`echo pipe3-m7q | cat | cat` then assert:

1. `pipe3-m7q` is **present** — it only arrives after traversing *two* pipes.
2. `cat: cannot open |` is **absent**.

Assertion 2 is what discriminates: under today's code the second `|` is handed to
`cat` as a filename and it prints exactly that error (to stderr since DDR-784), so
a broken implementation fails deterministically rather than by timing. Assertion 1
alone would not be enough — a single working pipe already prints the marker.

## Architecture prerequisite checklist

- **New syscalls / NSI:** none — `SYS_PIPE`/`SYS_FORK`/`SYS_DUP2`/`SYS_CLOSE`/
  `SYS_WAIT4` all ship. NSI stays at 75. **No kernel change.**
- TCB/roster, PMM/VMM, capability gates, AETHER, scheduler hooks, network policy,
  compositor, FS/on-disk format: **none**.
- **Capabilities:** unchanged.
- **New gate:** none — `smoke-shell` extended.
- **Security invariants:** **S2** — the stage loop is bounded by `argv[16]`
  (≤ 8 stages), each iteration consumes at least one token, and a malformed
  pipeline (`|` first, last, or doubled) is rejected before any fork. **S6** —
  each stage is its own process; a fault kills that child only, and the parent
  still reaps every child so no zombie accumulates. S1/S3–S5/S7/S8 not engaged.
  No invariant weakened, so no human sign-off required.

## Non-goals

Blocking pipe writes / >4 KiB streaming (kernel-side, separate slice), `2>&1`,
combining a pipeline with `<`/`>` redirection on the *same* line (the redirect
scan still applies to one command), job control, and exit-status propagation
(`$?`).
