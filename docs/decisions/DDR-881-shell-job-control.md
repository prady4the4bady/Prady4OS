= DDR-881 — PRISM job control: `&`, job table, `jobs`/`fg`, `%n` (item 34)

**Status:** Accepted
**Date:** 2026-08-09
**Scope:** `user/prism.c`, `smoke-shell`.

## Scope, stated before the code — and what is deliberately absent

Shipped: a trailing `&`, an 8-slot job table, `jobs`, `fg %n`, `kill %n`, and a
non-blocking reap that reports finished background jobs at the next prompt.

**Not shipped: `bg`, ^Z/SIGTSTP stop-and-continue, and terminal ownership.**
Those are not hard-to-reach corners — they need process **groups** and a tty
layer that owns a foreground group, and PRADYOS has neither `setpgid` nor a
controlling terminal. A shell offering `^Z` on a kernel with no job-control
signals would be advertising a capability the system cannot provide, and the
gate for it could only assert that a message was printed.

## The reap is not cosmetic

A background child that exits and is never waited for stays a **zombie holding
its TCB** (`sched.c` keeps exited threads in `THREAD_ZOMBIE` until a parent
collects them). The shell is the only thing that can collect its own children,
so `jobs_reap()` — `wait4(pid, &st, WNOHANG)` before each prompt — is what stops
`&` from leaking a TCB per background command.

`options` is `wait4`'s **third** argument, so the existing 3-argument `nsi()`
carries `WNOHANG` already. I briefly added a 4-argument wrapper for it and
removed it once I read `sys_wait4`'s signature: it would have been dead code
plus an unnecessary R10 constraint.

## Two ordering traps, both previously paid for

**`&` is stripped before redirection and pipe parsing.** Otherwise `>` sees it as
a filename and `|` sees it as a command. This is the same trap DDR-868 hit with
`2>&1`, which had to be matched before the generic `2>` prefix or it read as a
redirect to a file literally named `&1`.

**A `%n` that resolves to nothing must not fall through.** The first version
printed "no such job" and then continued with `pid` still 0. Guarded with an
explicit `resolved` flag.

## The gate, and what mutation testing changed about it

`smoke-shell` now runs `run /EXECTEST.ELF &`, `jobs`, `fg %1`, `jobs`, `kill %99`.

**`jobs` and `fg` output is deliberately NOT asserted.** `EXECTEST.ELF` finishes
in milliseconds, so whether `jobs` shows it Running or already reaped is a race.
Asserting on it would be a flaky gate dressed up as a feature test.

Three deterministic facts are asserted instead:

| Assertion | What it proves |
|---|---|
| `[1] <pid>` | `&` parsed; the shell forked and did **not** wait |
| `Done(0)   /EXECTEST.ELF` | the non-blocking reap ran and reported the status |
| `kill: no such job %99` **and** no `kill: pid 0 not found` | `%n` refused, and refused *without* calling kill |

The first attempt used `run /HELLO.ELF`, which reported `Done(127)` — my own
execve-failed code. HELLO.ELF is not on the shell's root; `/EXECTEST.ELF` is the
one the kernel places for exec tests. The gate found that, not review.

**Mutation matrix:**

| Mutation | Result |
|---|---|
| M1: ignore `&` (`background = 0`) | **killed** |
| M2: `kill %n` falls through with pid 0 | **killed — but only after the assertion was fixed** |

M2 initially **survived**. The assertion checked for the *message* `kill: no such
job %99`, which is printed before the guard is consulted, so it passed under the
mutation. Re-aimed at the behaviour — the absence of `kill: pid 0 not found` —
it kills M2.

Worth recording precisely because the first assertion looked reasonable: it
tested that the shell *said* the right thing, not that it *did* the right thing.

**And the guard is not oversold.** `kill(0)` is **not** a process-group broadcast
here — PRADYOS has no process groups, so the kernel finds no pid 0 and returns
an error. My first comment claimed a broadcast; that was wrong. The guard is
defence in depth against the day pid semantics grow, not a fix for a live
escalation.

Gates green: `smoke-shell`, plus the standard regression set. Zero warnings
under `-Werror`.

**Group 6 item 34 complete.**
