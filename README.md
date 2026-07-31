# PRADYOS — Sovereign Edition

A clean-room, bare-metal, AI-native operating system: the **NEXUS** kernel,
its bootloader, drivers, filesystem, userspace, and the **AETHER** agent
runtime. No code lineage from Linux/BSD. MIT licensed (see `LICENSE`).

> **Status: Phase 0 (toolchain + repo skeleton).** Nothing boots yet — that is
> Phase 1. See [`docs/build_status.md`](docs/build_status.md) for the live
> component tracker and [`docs/decisions/`](docs/decisions/) for architecture
> decision records (ADRs).

## Honest scope note

The full vision (kernel + drivers + custom FS + libc + Wayland compositor +
8-agent runtime + 4 architectures + quantum layer) is a multi-year, multi-person
effort. We build it phase by phase, each phase booting cleanly and passing its
tests before the next begins. Performance numbers in the source documents are
treated as **targets to be measured**, not facts — several are flagged for
revision in the ADRs. We never claim a benchmark we did not run.

## Build environment

- **Host:** WSL2 (Ubuntu). See ADR-001.
- **Toolchain:** LLVM/Clang + `ld.lld` + `llvm-objcopy`, NASM, Rust nightly
  (`x86_64-unknown-none`). Single source of truth: `tools/build/toolchain.mk`.

## Quickstart (run inside WSL2 Ubuntu, from the repo root)

```bash
make setup            # one-time: installs clang/lld/nasm/qemu/rust nightly
make toolchain-check  # compiles C + NASM + no_std Rust and links with ld.lld
make smoke            # QEMU boot test (SKIPs until Phase 1 produces an image)
```

If `make toolchain-check` exits 0, the toolchain is sound and we proceed to
Phase 1 (PRADYOS-BOOT).

## Layout

`arch/` architecture code · `kernel/` NEXUS core · `drivers/` · `fs/` (SFS, ext4,
vfs) · `aether/` agent runtime · `userspace/` (init, PRISM shell, libc) ·
`desktop/` compositor + UIs · `boot/` bootloader · `tools/` build + runners ·
`tests/` · `docs/`.
