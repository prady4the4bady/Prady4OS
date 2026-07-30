# DDR-805 — SIGPIPE: writing to a pipe nobody is reading must kill the writer

**Status:** design accepted; **implementation REVERTED — blocked, see
"What actually happened"**. No code from this DDR is in the tree.
**Date:** 2026-07-30
**Relates to:** DDR-787 (pipe refcount split), DDR-789 (`$?` in PRISM),
PROC-C (signal delivery).

## What is missing

`grep -rn SIGPIPE kernel/ user/` returns nothing — this is genuinely unshipped,
checked in the tree rather than taken from the roadmap.

Today, a ring-3 write to a pipe whose readers have all closed returns `-EPIPE`
(`sys_io.c:58`) and the writer **keeps running**. For a shell pipeline that is
the wrong shape: `producer | head -1` leaves `producer` writing into a ring that
can never drain, spinning until it finishes on its own. The kernel already knows
the write can never succeed — `pipe_readers() <= 0` is exactly that fact — and
then declines to act on it.

POSIX resolves this by raising `SIGPIPE` on the **writing thread**, whose default
action is to terminate. That is what makes `head` work as a pipeline stage rather
than merely as a program that stops printing.

## Decision

1. `SIGPIPE = 13` (the POSIX number; the tree already uses POSIX numbering for
   `SIGKILL 9`, `SIGUSR1 10`, `SIGTERM 15`, so an arbitrary value would be a
   trap for anyone reading `kill -l` output).
2. `signal_deliver`'s default-terminate set becomes `{SIGTERM, SIGPIPE}`.
   `SIGKILL` keeps its separate unblockable path above.
3. `sys_write` raises it on the reader-gone path, on `current_thread`.

Raising from a **syscall** context, not an ISR — this only sets a pending bit in
the caller's own TCB, so it takes no lock and S6 does not apply.

## The `total > 0` case — deliberately no signal

SIGPIPE is raised only where the call returns `-EPIPE`, i.e. when **nothing** was
written. A write that moved some bytes before the reader vanished returns the
short count and raises nothing.

This is not an accident of where the line sits. A partial write **succeeded** in
part; killing the writer would discard a result it legitimately produced, and the
short count is already the signal to the caller that the pipe is closing. The
existing `total > 0 ? total : -EPIPE` expression happens to encode exactly this
distinction, so the raise belongs on the `-EPIPE` branch only.

## Delivery timing, and why the writer still returns first

Signals are delivered from the IRQ return path to ring 3 (`idt.c`), not at the
syscall boundary. So the writer returns from `write()` with `-EPIPE` and is
terminated at the next timer-IRQ return. There is a bounded window where a
SIGPIPE-doomed thread executes a few more instructions.

That is correct rather than merely tolerable: a thread that installs a SIGPIPE
handler must observe `-EPIPE` from `write()`, which requires the syscall to
return normally. Terminating inside the syscall would make the handler case
unrepresentable. The window is bounded by one tick.

## Gate

`smoke-sigpipe` — a probe whose child writes to a pipe after the read end is
closed, asserting the writer dies **by signal** rather than by its own exit path.

The discriminating property is that the writer must not reach its own
"I finished" print. A gate that only checked "the write returned -EPIPE" would
pass against today's kernel, which already does that and is precisely the
behaviour being changed.

A/B arms, all of which must fail:
* **A** — drop `SIGPIPE` from the default-terminate set: the signal is pending,
  is ignored as a catchable-with-no-handler signal, and the writer runs to
  completion.
* **B** — do not raise it in `sys_write`: `-EPIPE` still returns, writer lives.
* **C** — raise it on the `total > 0` branch as well: the partial-write case
  starts killing writers that legitimately produced output, which the probe's
  second phase detects.

Arm C is what stops this DDR from being "add a signal somewhere in the vicinity".

## What actually happened — the design is right, the tree is not ready

The change above was implemented exactly as designed (`SIGPIPE 13`, added to the
default-terminate set, raised on the `-EPIPE` branch only), built warning-free,
and was then **reverted before commit** because it broke a green gate.

`smoke-shell` FAILED. The DDR-786/787 200-line pipeline test truncated:

```
pipe payload line 196 0123456789abcdef
pipe payload line 197 01[user] sys_exit(0) — thread terminating
```

— mid-line at 197 of 200, and the serial log ends there. `smoke-syspipe`,
`smoke`, and `smoke-user` all still passed, so the damage is specific to the
shell's multi-stage pipeline, not to pipes generally.

**The mechanism is NOT yet named, and that is why nothing was committed.** The
evidence is consistent with at least two different stories — the writer being
killed while the reader was still consuming, or the reader stopping first and the
truncation being a consequence rather than the cause — and the log ending at that
point does not distinguish them. Committing a fix on either story would be the
fifth blind fix this tree has refuted.

### What the next session must do first

1. Re-apply the three edits (they are small and fully described above).
2. Instrument the death, do not infer it: print the pid and signal in
   `signal_deliver`'s default-terminate branch, and print `pipe_readers()` at the
   `-EPIPE` site. The question to answer is **which thread** takes SIGPIPE and
   **what the reader count actually was** at that moment.
3. Only then decide between the two candidate mechanisms:
   * PRISM's pipeline closes the read end before the writer has drained, in
     which case POSIX genuinely says kill the writer and the **200-line gate's
     expectation is what is wrong** — it would be asserting behaviour that only
     held because SIGPIPE was missing;
   * or the writer is being signalled while a reader is still live, in which case
     the raise site or `pipe_readers()` accounting is wrong.

These have opposite fixes. That is exactly why the mechanism has to be named
before either is written.

### Note for whoever picks this up

Do not "fix" this by leaving SIGPIPE out of the default-terminate set. That
disables the entire feature to make a gate pass, which is the S11 failure mode in
its purest form. The gate that broke is the most valuable output of this slice —
it found a real interaction on the first run.

## CORRECTION — SIGPIPE was not the cause

Everything in the section above attributes the `smoke-shell` failure to this
DDR's change. **That attribution is wrong.** After reverting all three edits and
rebuilding, `smoke-shell` fails again at the identical point:

```
pipe payload line 197 01make: *** [Makefile:738: smoke-shell] Error 1
```

Same line, same truncation, with none of this DDR's code in the tree. The
failure is independent of SIGPIPE.

I came within one commit of "fixing" a defect I had not caused, and of recording
a mechanism for it that could not have been true. The revert was still the right
call — committing an unverified change on top of an unexplained red gate would
have fused two unrelated problems — but the reasoning that motivated it was
mistaken, and the check that caught it was simply re-running the gate *after*
the revert instead of assuming the revert had worked.

**Generalisable rule: a revert is not verified until the gate is re-run.** A
failure that persists through a revert was never yours.

### Actual status of the `smoke-shell` failure

Unattributed. Known facts only:

* it passed in CI at `90634b6` (run 30483750211, green), and `smoke-shell` does
  run in CI (`.github/workflows/*.yml:99`);
* it fails locally at `9f1459a` (DDR-804) with SIGPIPE absent;
* `smoke-syspipe`, `smoke`, `smoke-user`, and the four egress gates all pass;
* CI run **30504947387** on `9f1459a` was in progress when this was written and
  is the discriminator between "DDR-804 regressed it" and "local-environment
  flake". The gate is driven by fixed `sleep` intervals against a FIFO, so it is
  more timing-sensitive to host load than the `boot_test.sh` gates.

Read that run's verdict before investigating anything else. If CI is green, this
is a local timing artefact and DDR-805 can proceed on a quiet machine; if CI is
red, DDR-804 is the first suspect and DDR-805 stays blocked behind it.
