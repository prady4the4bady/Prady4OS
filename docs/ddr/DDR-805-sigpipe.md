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

## OPEN-8 RESOLVED TO A CAUSE — DDR-804 is the regression, and two of my
## intermediate readings were wrong

Bisect, deterministic:

| tree | result |
|---|---|
| `90634b6` (CI-green) | **PASS** |
| `6c375ea` HEAD, run 1 | FAIL |
| `6c375ea` HEAD, run 2 | FAIL |

`6c375ea` is docs-only, so **`9f1459a` (DDR-804) is the regressing commit**. Not
a local timing artefact, not SIGPIPE. I shipped this.

### Correction 1 — the "truncation at line 197" never happened

Every earlier note here describes the 200-line pipeline truncating at 197/200.
It does not. That line is split by an interleaved `[user] sys_exit(0)` from
another thread; lines 198, 199, 200 and `BIGTAIL-e5v` all follow it. **The pipe
test passes.** I read a concurrency artefact in a shared console as data loss and
built two hypotheses on it.

### Correction 2 — I analysed the wrong log

The `build/shell_serial.log` I then inspected was written by the **passing**
`90634b6` run, because the bisect script ran that arm last. Everything looked
healthy because it was healthy — it was not the failing artefact. Same class of
mistake as DDR-791's byte-identical arms: reasoning confidently about an artefact
that was not the one under test.

### The actual failing assertion

```
[shell] FAIL: $? did not expand to 0 after a successful command (DDR-789)
```

DDR-804 broke `$?` expansion in PRISM. The mechanism is **not yet named**, and
per S5 no fix goes in before it is. Candidates, in the order worth testing:

1. **Kernel image growth.** `privacynettest.elf` (~6.4 KB) was added to the
   `user_image.asm` incbin set, so the embedded image grew on every boot, gated
   or not. The tree has a standing low-memory image cap, and exit-status
   plumbing sits near init/reap paths — worth checking whether something now
   straddles a boundary.
2. **`fwcfg_init()` at boot.** It runs unconditionally and performs bounded port
   I/O before `acpi_init()`. It should be inert, but it is new on every path.
3. The gated spawn block itself — least likely, `probe_enabled("privnet")` is
   false without `QEMU_PROBES`.

Discriminate by reverting each independently, not together: rebuild with only the
incbin entry removed, then with only `fwcfg_init()` removed. One of them restores
`$?`, and that names the mechanism.

**Do not revert DDR-804 wholesale to go green.** It closes OPEN-7 and carries a
verified three-arm A/B; the defect is a side effect of how it was wired in, not
of the mechanism.

## OPEN-8 MECHANISM LOCALIZED — `fwcfg_init()`, and CI agrees

CI independently confirms the bisect: run **30504947387 on `9f1459a` = failure**.

Discriminating experiment, one variable changed:

| arm | kernel | `smoke-shell` |
|---|---|---|
| `fwcfg_init()` removed, incbin/image growth left in | `3d678e255ab7` | **PASS** |

So **`fwcfg_init()` is the mechanism** and the ~6.4 KB image growth from the
`privacynettest.elf` incbin is exonerated. Candidate 1 needs no further testing.

### What is still NOT explained

Localizing is not explaining, and the fix must wait on the explanation (S5).
`fwcfg_init()` only performs bounded port I/O into a 257-byte static before
`acpi_init()`. Two live hypotheses, and they have different fixes:

1. **Boot-timing shift.** `smoke-shell` is driven by fixed `sleep 0.5` intervals
   against a FIFO after `PRISM_READY`, and the `$?` assertion is order-sensitive.
   If the fw_cfg directory is large, the scan is up to 64 entries x 64 bytes =
   4096 `inb`, each a VM exit under TCG. If that is the cause, **the gate is
   timing-fragile and the gate is what needs fixing** — not the driver.
2. **Something in the port I/O genuinely perturbs guest state** on this machine
   type. Less likely, more serious.

### The measurement that settles it

Instrument `fwcfg_init()` to print, once: whether the signature matched, the raw
directory `count` before clamping, and the number of `inb` performed. That
distinguishes "a handful of reads" (hypothesis 1 dies, look harder at 2) from
"thousands of reads" (hypothesis 1 confirmed; fix the gate's fixed-sleep driver,
or make the scan stop at the matching entry instead of draining the directory).

Note the scan currently keeps reading after it finds its entry, deliberately, to
leave the device in a defined state. If the read volume turns out to be the
problem, that decision is the thing to revisit — not the feature.

**DDR-804's mechanism stays.** It closes OPEN-7 and carries a verified three-arm
A/B. What is in question is only the unconditional boot-time scan.

## §Design (re-confirmed against the tree, 2026-07-31)

The three edits stand unchanged. What needed re-checking was the **gate**, and the
tree does not support the assertion as originally specified.

### Blast radius

* Pipe write path: `kernel/syscall/sys_io.c:58` — `return total > 0 ? total : -EPIPE;`
  inside `fd_write_user`'s `FD_PIPE` branch. This is the only `-EPIPE` producer.
* Gates exercising pipe I/O: `smoke-syspipe` (Makefile:1889) and `smoke-shell`
  (the DDR-786/787 200-line pipeline plus `|`/`| cat | cat` cases). Both must
  stay green — `smoke-shell` in particular now has a trustworthy baseline for
  the first time (DDR-809).
* Signal path: `signal_deliver` (`kernel/proc/signal.c:72`), default-terminate
  set currently `{SIGKILL, SIGTERM}`.

### The gate cannot assert "exit status 13", and why

`sched_exit(-1)` sets `exit_status = -1` for **every** default-terminate signal;
`sys_wait.c:66` hands that to `wait4`, and PRISM surfaces it as `$?`
(`user/prism.c:103`). The kernel does not encode *which* signal terminated a
process anywhere — there is no `128+signum` convention in this tree.

Asserting `$? == 13` would therefore require a **fourth** edit adding that
encoding, which changes the observable exit status of SIGKILL and SIGTERM as
well. That is a behavioural change to two signals this DDR is not about, with
blast radius into any gate asserting on the current value. It is a legitimate
future change; it is not part of "three edits only", and folding it in silently
would be exactly the kind of scope drift the A/B discipline exists to catch.

### What the gate asserts instead — and why it is still discriminating

The discriminating property is **"the writer does not survive its own write"**,
which needs no new encoding:

* the probe writes to a readerless pipe and, on the next line, prints
  `PRADYOS_SIGPIPE_STUB`;
* if SIGPIPE terminates it, that line is never reached;
* if SIGPIPE is missing or not in the default-terminate set, `write()` returns
  `-EPIPE`, the probe survives, and the line prints.

`PRADYOS_SIGPIPE_STUB` is therefore the `FORBIDDEN_SENTINEL`, and it is
load-bearing rather than decorative: a stub that fakes success cannot avoid
printing it, because printing it *is* what surviving means.

**Mechanism metric:** `$?` for the writer must be `-1` (kernel signal
termination) and not `0`. That separates "terminated by signal" from "exited
normally" using only what the tree already represents. A control arm — the same
write with a **live** reader — must print the marker and yield `$? == 0`, so the
gate also proves the kill is specific to the readerless case rather than a
blanket failure of pipe writes.
