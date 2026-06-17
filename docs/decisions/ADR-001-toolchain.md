# ADR-001: Build environment and cross toolchain

- **Status:** Accepted
- **Date:** 2026-06-17
- **Phase:** 0

## Context

The host is Windows 11. PRADYOS targets x86_64 first, with aarch64 and riscv64
required later (Instructions §Chipset Variant System). Native OSDev tooling
(OVMF, mtools, xorriso, disk-image work) is painful on Windows. We need a
reproducible cross-build path and an assembler + a safe-systems language (Rust)
per the blueprint.

## Decision

- **Build environment: WSL2 (Ubuntu).** Confirmed with the user.
- **Toolchain: LLVM/Clang** (`clang`, `ld.lld`, `llvm-objcopy`) for C and
  linking, **NASM** for assembly, **Rust nightly** with the built-in
  `x86_64-unknown-none` bare-metal target for safe subsystems. Confirmed with
  the user.

Rationale: one Clang install cross-compiles all three target architectures via
`--target=`, avoiding a multi-hour per-arch GCC build and matching the
4-architecture goal. NASM is the blueprint's specified assembler.

## Alternatives considered

- **GCC cross-compiler (`x86_64-elf-gcc`)** — classic OSDev path, very mature,
  but requires building binutils+gcc per architecture. Rejected for setup cost
  and multi-arch friction.
- **Native Windows (clang + QEMU-for-Windows)** — no VM, but disk-image/UEFI
  tooling is painful on Windows. Rejected.
- **Docker only** — most reproducible; kept for CI, but WSL2 gives a faster
  interactive edit→boot→debug loop for day-to-day kernel work.

## Consequences

- The x86_64 freestanding triple is `x86_64-elf` (see
  `tools/build/toolchain.mk`). **VERIFIED & PINNED 2026-06-17** on WSL2
  Ubuntu-22.04 with clang 14.0.0: `make toolchain-check` compiled C + NASM +
  no_std Rust and linked them with `ld.lld` (exit 0). `clang --target=x86_64-elf
  -dumpmachine` normalizes to `x86_64-unknown-unknown-elf`, which is correct for
  freestanding. Fallbacks (unused): `x86_64-unknown-none-elf`,
  `x86_64-pc-none-elf`. `llvm-objcopy` (unversioned) confirmed present for the
  Phase-1 flat-binary step.
- **Repo location:** the repo lives on the Windows filesystem and is built from
  WSL via `/mnt/c/...`. This is convenient but **slower than a WSL-native path**
  and prone to CRLF issues — mitigated by `.gitattributes` forcing LF. If
  Phase 2+ builds become slow, relocate to `~/pradyos` inside the WSL
  filesystem. Revisit then.
- CI (`.github/workflows/ci.yml`) reproduces this toolchain on `ubuntu-latest`.
