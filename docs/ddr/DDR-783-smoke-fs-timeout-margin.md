# DDR-783 — `smoke-fs` asserts the boot chain's LAST sentinel under the default 30 s

**Status:** implemented (one-line gate fix) + a systemic finding recorded as a
proposal, deliberately NOT applied. Infrastructure; supports all of Section B.

## Symptom

CI run **30192189559** (`09d3525`) failed at step 10, `smoke-fs`:

```
[smoke] FAIL — required pattern 'compress/readback/tag OK' not found
```

The serial log showed the self-test running *normally* — FAT mount, reads,
`/KOUT.TXT` write+readback, `created+deleted /TMP.TXT OK`, `[sfs] journal
abort/commit/replay OK`, `[sfs] snapshot version-isolation OK` — and then simply
ending. The very next line in `kmain` is the missing one.

## Not a DDR-782 regression — established, not assumed

- DDR-782 changed `sys_open` and the FD_VFS write path. `sfs_selftest_lz4` is a
  **kernel-internal** test that never goes through the fd/syscall layer, so there
  is no path from the change to this test.
- **The identical image passes `smoke-fs` locally**, `compress/readback/tag OK`
  included. A real regression would not be host-dependent.

## Root cause — measured, not guessed

Instrumented boot, timestamping each milestone from qemu start:

| Milestone | t (s) |
|---|---|
| `NEXUS KERNEL OK` | 0.31 |
| `PRISM_READY` | 23.91 |
| `[sfs] journal abort/commit/replay OK` | 24.09 |
| `[sfs] snapshot version-isolation OK` | 24.18 |
| **`[sfs] lz4+tags compress/readback/tag OK`** | **24.26** |

`smoke-fs` runs at the harness **default `TIMEOUT_S=30`** (`boot_test.sh:19`).
So the last required sentinel lands with **5.7 s of margin (19 %) on a fast local
machine**. A GitHub runner is routinely slower than local, so the gate is
genuinely marginal and will flake intermittently — this run is that flake, not a
new defect.

**The inconsistency that makes it a defect rather than bad luck:** `smoke-user`
asserts the *same* `[sfs] lz4+tags compress/readback/tag OK` string and already
runs at `TIMEOUT_S=60` (Makefile:582). `smoke-fs` asserting the chain's latest
sentinel under the default was an oversight — the SFS chain grew across slices
4g/4h/4i and DDR-760 while this gate's window never moved.

## Decision — raise `smoke-fs` to `TIMEOUT_S=60`

2.5x the measured 24.3 s, and identical to the sibling gate asserting the same
sentinel. **This cannot mask a hang:** `boot_test.sh` greps *after* the window
regardless, so a genuinely hung kernel still produces no sentinel and still
fails. The change buys margin, not silence.

Only this gate is changed. The other 56 default-30 gates assert earlier
sentinels; changing them without per-gate evidence would be exactly the blind
tuning this project forbids.

## Systemic finding — proposal, NOT applied

`boot_test.sh` **always runs the full `TIMEOUT_S` window and only then greps** —
it never exits early once every sentinel has been seen. Two consequences:

1. Every gate's timeout must be hand-tuned tightly, because the timeout *is* the
   runtime. That is why a growing boot chain silently eats margin.
2. Wall-clock is the sum of all timeouts, not of the work.

An early-exit harness (stream the serial log, exit 0 as soon as all required
sentinels have appeared and no forbidden one has) would remove the whole class of
flake *and* make CI faster — a generous timeout costs nothing when it is
satisfied early. Not applied here because it touches the shared harness behind
100+ gates and deserves its own slice with its own evidence, and because
`FORBIDDEN_SENTINEL` semantics need care: a forbidden pattern that would have
appeared *after* an early exit must still fail the run, so early exit is only
sound once the forbidden set is proven to be prefix-stable, or the exit is
delayed to a bounded settle window.

## Architecture prerequisite checklist

- Kernel code, syscalls/NSI, TCB, PMM/VMM, capabilities, AETHER, scheduler hooks,
  FS/root-mount, compositor: **none touched**. This is a test-harness parameter.
- New gate: none. Gate count unchanged: **106**.
- **Security invariants:** none engaged. No kernel or user code changes, so
  S1–S8 are untouched. Specifically **not** an S2 concern: the timeout does not
  bound any kernel loop, and a hang still fails the gate.
