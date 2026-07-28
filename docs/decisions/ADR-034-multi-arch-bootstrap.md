# ADR-034 — multi-architecture bootstrap (aarch64, riscv64)

**Status:** accepted; boot slice implemented and CI-green on both
architectures (run 30381074300).
**Date:** 2026-07-28
**Supersedes:** nothing. **Amends:** nothing binding.
**Relates to:** `docs/platform_profiles.md`, ADR-033 (toolchain sourcing).

## Decision 1 — clang is the cross-compiler; no GCC cross-toolchain

The kernel already builds with `clang` + `ld.lld` (+ `nasm` for x86 assembly).
clang is a cross-compiler by construction: `--target=aarch64-none-elf` and
`--target=riscv64-none-elf` need no separate binutils install, and `ld.lld`
links both. Verified locally — `clang --print-targets` lists `aarch64` and
`riscv64`, and `ld.lld` is present.

Adding `aarch64-linux-gnu-gcc` / `riscv64-linux-gnu-gcc` would mean three
compilers with three sets of warning behaviour to keep at zero under `-Werror`,
and two more apt packages on every CI run. The rejected option is the more
conventional one, which is why the reason is recorded rather than assumed.

**Consequence:** assembly for the new architectures is GAS syntax in `.S` files
compiled through clang's integrated assembler, not `nasm`. `nasm` remains
x86-only.

## Decision 2 — the new ports are ADDITIVE; x86_64 is not restructured

The obvious move is to introduce `kernel/arch/<arch>/` and hoist the existing
x86_64 code into it. This ADR explicitly does **not** do that yet.

x86_64 is the reference platform, it is CI-green, and it currently carries an
unresolved intermittent (BUG-1, DDR-777/791). Restructuring a hundred files
underneath an open intermittent would make the next failure impossible to
attribute: a red gate could be the bug or the move, and no evidence would
separate them. So the new architectures get their own directories and their own
build targets, and **nothing in the x86_64 path is touched**.

The hoist happens later, as its own slice, once BUG-1 is closed. That ordering is
the decision; recording it prevents the tree from drifting into a half-done
abstraction that nobody planned.

## Decision 3 — scope of the first slice is BOOT, not parity

This slice delivers, per architecture:

* a bootstrap that reaches C,
* a working serial console,
* the `NEXUS KERNEL OK` sentinel,
* a `make kernel-<arch>` build target and a `make smoke-<arch>` boot gate,
* a CI job.

It does **not** deliver the ported smoke-gate set. Section 1 [B]/[C] of the
phase-2 directive asks for "all existing smoke gates ported and green", and that
is a much larger body of work: the gates exercise the PMM, VMM, scheduler, VFS,
virtio, and the syscall surface, each of which is x86_64-specific today and each
of which needs its own port and its own slice.

Claiming otherwise would put a green tick against work that does not exist. The
tracker records this slice as **boot-only**, and the remaining gates as
outstanding.

## Platform facts this bootstrap relies on

Stated explicitly so a later reader can check them rather than trust them. All
concern the QEMU `virt` machine, which is the CI target; none are claims about
physical hardware.

**aarch64 (`-machine virt -cpu cortex-a72`)**
* RAM base `0x4000_0000`. The image links at `0x4008_0000`, the conventional
  load offset.
* PL011 UART0 at `0x0900_0000`. `DR` at offset `0x00`; `FR` at offset `0x18`;
  `FR` bit 5 = `TXFF` (transmit FIFO full).
* `-kernel <ELF>` honours the ELF's own load addresses and entry point.

**riscv64 (`-machine virt -cpu rv64`)**
* OpenSBI (`-bios default`) occupies `0x8000_0000` and hands off to the payload
  at `0x8020_0000` in **S-mode**, with `a0` = hartid and `a1` = DTB pointer.
* NS16550A UART at `0x1000_0000`. `THR` at offset 0; `LSR` at offset 5;
  `LSR` bit 5 (`0x20`) = transmit-holding-register empty.

## What I am uncertain about — read the CI result, not this list

Per the phase-2 self-answer rule, the minimum viable bootstrap ships and CI
adjudicates. These are the specific things a failure would most likely be:

1. **aarch64 exception level on entry — now MEASURED, not assumed.** The first
   version printed "EL1" as a hard-coded string, which asserted the answer
   instead of finding it. `arch_main` now reads `CurrentEL[3:2]` and prints what
   it actually reads, so the CI log is evidence rather than a restatement of the
   guess. The bootstrap still does not switch EL or configure `SCTLR_EL1` beyond
   the reset state; that matters for the later MMU work, not for the console.
2. **Secondary harts / CPUs.** Both bootstraps park every core except the
   primary. On riscv64 the parking is decided from `a0` before the stack is set
   up. If OpenSBI on the CI image starts only hart 0, the parking branch is
   simply never taken — harmless either way.
3. **Cache/MMU state at entry.** Nothing here enables the MMU or caches, so the
   UART MMIO is accessed with the reset attributes. That is fine for a console
   and is *not* fine for the memory subsystem the later slices will add.
4. **`-cpu rv64` naming.** RESOLVED — accepted by the CI QEMU; riscv64 booted
   first try (run 30380921699).

## What CI actually found

Both failures were environmental, not bootstrap defects, and both are worth
recording because neither was on the uncertainty list:

* **`ld.lld` is not preinstalled on `ubuntu-latest`** (run 30380714176). Worse,
  the job's own toolchain step ran `ld.lld --version | head -1` and PASSED — a
  pipeline returns its LAST command's status, so `head` masked the missing
  binary and the failure surfaced two steps later as an opaque make error. The
  check now uses `command -v` under `set -e`.
* **QEMU's `virt` machine instantiates a default virtio NIC** whose option ROM
  (`efi-virtio.rom`) is absent from Ubuntu's `qemu-system-arm` package, killing
  QEMU at startup with an empty serial log (run 30380921699). Fixed with
  `-nodefaults` rather than by shipping the ROM: this gate boots a CPU, some RAM
  and a UART, so creating a NIC it never touches was the actual mistake.

Uncertainty 1 is the one most likely to matter later; it is cheap now and
expensive to discover after the MMU work is written on top of it.
