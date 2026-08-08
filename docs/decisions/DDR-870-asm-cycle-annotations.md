# DDR-870 — cycle annotations backed by a real benchmark (Group 8 items 44/45)

**Status:** Accepted
**Date:** 2026-08-08
**Scope:** `user/benchtest.c`, `smoke-bench`, annotations on
`arch/x86_64/context.asm` and `arch/x86_64/syscall_entry.asm`.

## The decision that shapes everything else

Item 44 asks for "cycle-count annotations". **Under QEMU TCG there is no such
thing as a cycle count.** The guest TSC counts emulated time: a dynamically
translated `swapgs` costs whatever its translation costs, not what the
instruction costs on silicon. Writing "context switch = N cycles" from a TCG
run would be inventing precision the measurement cannot support, and it would
be quoted later as though it were hardware truth.

So each annotation carries **two** figures, separated and labelled:

- **STATIC** — instruction count, stack traffic, moves, serialising
  instructions. Derived from the code, exact, and true on real hardware.
- **MEASURED** — the TCG figure, explicitly marked *emulated ticks, NOT
  hardware cycles*, with a note that it is valid for regression detection and
  worthless as an absolute claim.

Item 45's "documented cycle counts" is satisfied by the static analysis plus an
honestly-labelled measurement, not by a number that reads authoritative and
isn't.

## Measured (QEMU TCG, 2000 iterations, minimum, baseline-corrected)

```
rdtsc_base=119   syscall_getpid=662   syscall_yield=170660
```

- **Minimum, not mean.** The minimum is the closest thing to an uninterrupted
  path; the mean folds in timer ticks and preemption unrelated to the code.
- **Baseline-corrected.** The RDTSC pair costs 119 ticks itself; reporting a
  syscall figure that silently included it would overstate the path.
- **`SYS_GETPID` on purpose** — its handler does almost nothing, so the figure
  is the trampoline plus dispatch rather than a syscall's work.
- **`LFENCE` before `RDTSC`** — without it an out-of-order core may retire the
  timestamp read early and the measurement silently shrinks.

## Static analysis (the part that holds on hardware)

| path | instructions | stack traffic |
|---|---|---|
| `context_switch` | 17 | 14 accesses / 112 B |
| syscall entry+exit | 46 | 18 accesses / 144 B |

The annotations explain *why* each set is the size it is — the nine saved
registers in the syscall path are exactly those the C dispatch may clobber, and
both fewer (breaks the ABI promise) and more (two dependent memory ops per call
on the hottest path) are wrong. That reasoning is what makes the annotation
useful to the next person; the raw count alone would not be.

## The gate asserts consistency, not a threshold

`smoke-bench` checks the benchmark **ran** and produced self-consistent numbers
— it does not assert any particular figure. A hard cycle threshold under TCG
would fail on a faster host and pass on a slower one for reasons unrelated to
the code, which is a flaky gate wearing a performance-budget costume.

The probe does assert `syscall > rdtsc_baseline`, because a measurement at or
below its own noise floor is not a measurement.

Opt-in via DDR-804 (`QEMU_PROBES=bench`): it issues thousands of syscalls and
would perturb the timing-sensitive gates if it ran on every boot.

## A note on how this nearly went wrong

An earlier attempt at this item reported `smoke-bench` **passing** when the
probe was not spawning at all. Two separate reading errors combined: stale
build objects (fixed by DDR-867's `--checksum --no-times` mirror) and unreliable
`$?` capture through inline `wsl bash -c`. The same unreliable capture briefly
suggested `boot_test.sh` never failed on a missing sentinel — a finding which,
had it been true, would have invalidated the entire gate suite. Re-running from
a **script file** showed `exit=1`, correctly.

The rule earned: **when a measurement implies something catastrophic,
re-measure by a different route before believing it.**

## Verification

3/3 consecutive `smoke-bench` passes. Regression green: `smoke-bench`, `smoke`,
`smoke-ftruncate`, `smoke-shell`, `smoke-user`. Zero warnings under `-Werror`.
136 gates across 6 shards.

**Group 8 items 44 and 45 complete.** Items 42 (`fast_memcpy.asm`) and 43
(`ipc_copy.asm`) remain — the benchmark built here is what they will be measured
against.
