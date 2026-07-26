# DDR-785 — `boot_test.sh` early exit once every required sentinel is present

**Status:** implemented — verified locally. The harness self-test passes all four
cases with the right *timings* (early exit finished in **2 s** against a 60 s
window; the late-forbidden case took the full window and **failed**, as it must).
End-to-end on real gates: **`smoke-fs` 30 s** (60 s window) and
**`smoke-uaccess` 4 s** (30 s window), both still PASS. Infrastructure; the systemic finding
recorded in **DDR-783** and deferred there for its own slice. Supports all of
Section B delivery (every gate runs through this harness).

## Problem — the timeout *is* the runtime

`tools/qemu_runner/boot_test.sh` runs `timeout "$TIMEOUT_S" qemu-system-x86_64 …`
to completion **every time**, and only then greps the serial log. A gate that
satisfies all its assertions at t=3 s still burns its whole window. Two
consequences:

1. **Every gate's timeout must be hand-tuned tightly**, because generosity costs
   wall-clock directly. That is exactly how DDR-783 happened: the SFS self-test
   chain grew, `smoke-fs` kept the default 30 s while its last sentinel moved to
   t=24.3 s, and the gate started flaking on slower runners.
2. **CI wall-clock is the sum of timeouts, not of work.** Measured across the
   Makefile:

   | | gates | timeout budget |
   |---|---|---|
   | all `boot_test.sh` invocations | 91 | **7590 s = 126.5 min** |
   | declare `FORBIDDEN_SENTINEL` | 38 | 3540 s |
   | **no forbidden patterns** | **53** | **4050 s** |

   At a conservative ~28 s of real boot per gate, those 53 eligible gates spend
   ~1484 s doing work and ~2566 s waiting — **~43 min of pure idle per CI run**.

## The correctness hazard, and why this design avoids it rather than mitigating it

Early exit is **not** semantics-preserving in general. `FORBIDDEN_SENTINEL`
patterns must *not* appear, and today they get the full window to show up.
Exiting early would only prove "it had not appeared yet" — a gate that should
FAIL could PASS. That is a false-negative, the dangerous direction, and a
"bounded settle window" is a heuristic that cannot rule it out.

**Decision: early-exit only when `FORBIDDEN_SENTINEL` is empty.** Then the
verdict is provably unchanged — the only assertions in play are "these patterns
must appear", and appearing earlier cannot turn a PASS into a FAIL or vice
versa. The 38 gates that declare forbidden patterns keep today's behaviour
**exactly**, byte for byte.

This deliberately leaves some savings on the table (3540 s of forbidden-gate
budget) in exchange for a guarantee rather than an argument. A later slice may
extend coverage with per-gate evidence — e.g. many forbidden patterns here are
`X FAIL` paired with a required `X OK` from the same self-test line, which is
plausibly mutually exclusive, but "plausibly" is not a basis for weakening a
gate.

## Design

QEMU already writes the serial stream to a **file** (`-serial "file:$SERIAL_LOG"`),
not a pipe, so the log can be polled while the guest runs — no buffering or
line-discipline problem.

- Launch qemu in the background under the same `timeout "$TIMEOUT_S"`, so the
  hard ceiling is unchanged.
- When early exit is eligible (no forbidden patterns), poll every 0.25 s: once
  `$SENTINEL` **and** every non-empty `EXTRA_SENTINEL` line are present in the
  log, terminate the guest and proceed to the existing verification block.
- When not eligible, wait for qemu exactly as today.
- **The verification block is untouched** and still runs against the captured
  log, so PASS/FAIL output, the forbidden check, and the delete-log-on-PASS
  behaviour are identical. Early exit only changes *when we stop capturing*,
  never how the verdict is computed.
- On timeout or a missing sentinel the behaviour is unchanged: the full window
  elapses and the existing FAIL path prints the serial output.

## Gate — discriminating, and it tests the dangerous direction

A harness change must be tested on the harness, not only through the gates it
runs. `make smoke-selftest` (host-only, no QEMU) drives `boot_test.sh` against
synthetic serial logs via a stub, asserting:

1. **Early exit happens when eligible** — all sentinels present ⇒ PASS, and the
   elapsed time is well under `TIMEOUT_S` (proves it did not wait out the window).
2. **A late forbidden pattern still FAILS** — the case early exit could have
   broken. With `FORBIDDEN_SENTINEL` set, the run must take the full window and
   must FAIL. *This is the assertion that would catch a wrong implementation*: a
   naive early exit passes this test only by accident of timing, and fails it
   deterministically when the forbidden line arrives late.
3. **A missing required sentinel still FAILS** and still prints the log.

## Architecture prerequisite checklist

- Kernel code, syscalls/NSI, TCB, PMM/VMM, capabilities, AETHER, scheduler hooks,
  FS/root-mount, compositor, on-disk format: **none touched**. This is the test
  harness only.
- Gate count: unchanged at 106 for the boot gates; `smoke-selftest` is a new
  host-only self-check of the harness, not a new kernel gate.
- **Security invariants:** none engaged — no kernel or user code changes, so
  S1–S8 are untouched. Explicitly **not** an S2 matter: the `timeout` ceiling is
  unchanged, and a hung kernel still fails exactly as before (no sentinel).
  Critically, this slice does **not** weaken any gate: the one way it could have
  (missing a late forbidden pattern) is excluded by construction, not by
  argument.

## Non-goals

Extending early exit to gates with forbidden patterns, changing any gate's
`TIMEOUT_S`, touching the interactive `smoke-shell`/`smoke-fs-*` recipes that
drive QEMU directly rather than through this harness, and parallelising CI.
