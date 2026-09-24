# PRADYOS — Sovereign Edition

A bare-metal, AI-native operating system for x86_64: the **NEXUS** kernel, its
BIOS and UEFI bootloaders, drivers, filesystems (SFS, FAT32, read-only ext4),
the **PRISM** shell and userspace, a framebuffer compositor, and the **AETHER**
agent runtime.

## Licence

**PradyOS is proprietary software. All rights reserved.** See [`LICENSE`](LICENSE)
— it is the sole and authoritative statement of your rights, and it grants none
without a separate written agreement with the owner. Any earlier statement in
this repository that PradyOS is MIT-licensed is **wrong and withdrawn**
(operator decision, 2026-09-24).

PradyOS bundles two third-party components under their own licences: **lwIP**
(BSD-3-Clause, linked into the kernel) and **musl libc** (MIT with
BSD-licensed portions, linked into user programs). Their notices ship on the
ISO as `THIRD_PARTY_NOTICES.txt`, generated from `third_party/` at build time
(`tools/build/mk_notices.sh`). The kernel itself is written for this project;
it contains no Linux or BSD kernel code.

## Status

**The system boots and runs.** One hybrid ISO boots through both BIOS (El
Torito hard-disk emulation) and UEFI to a live userspace: an SFS root, the
PRISM shell, and the AETHER daemon with an agent. `smoke-iso-x86` and
`smoke-iso-userspace` check this on every CI run. About 180 QEMU gates run in
CI across ten shards.

**`v1.0.0` is not yet tagged.** The release is held pending the operator's
sign-off on the go/no-go list in
[`docs/PRE_LAUNCH_CHECKLIST.md`](docs/PRE_LAUNCH_CHECKLIST.md) §0, which also
lists the known open issues. Other status documents:

- [`docs/build_status.md`](docs/build_status.md) — component tracker
- [`docs/decisions/`](docs/decisions/) and [`docs/ddr/`](docs/ddr/) — ADRs and DDRs

aarch64 and riscv64 are **boot stubs only** (ADR-034): they print a sentinel and
halt. No ISO is built for them.

## Honest scope note

Performance numbers in the design documents are **targets**, not facts. Under
QEMU TCG the guest cycle counter measures emulated time, so no hardware speedup
is claimed from it. Every claim in `docs/` is meant to rest on a measurement or
a gate. Where one does not, the document should say so.

## Build environment

- **Host:** Ubuntu 24.04 (native or WSL2). See ADR-001.
- **Toolchain:** LLVM/Clang, `ld.lld`, `llvm-objcopy`, NASM, `xorriso`, QEMU and
  OVMF. `tools/build/toolchain.mk` is the single source of truth.

```bash
make setup            # one-time: install the toolchain
make image            # build the kernel and disk image (-Werror, zero warnings)
make smoke-shell      # boot in QEMU and drive the PRISM shell
make smoke-iso-x86    # build the hybrid ISO and boot both arms
```

`make iso` writes `build/pradyos.iso` and `build/pradyos.iso.sha256`. To verify
a downloaded ISO, put both files in one directory and run
`sha256sum -c pradyos.iso.sha256`. This detects corruption; it does **not**
prove who built the image, because the ISO is not signed yet.

## Layout

- `boot/` — stage1/stage2 BIOS loader and the UEFI loader
- `kernel/` — NEXUS: scheduler, MM, drivers, VFS, syscalls, crypto, AETHER core
- `user/` — ring-3 programs (PRISM, compositor, terminal, agents, gate probes)
- `third_party/` — lwIP and musl
- `tools/` — build, QEMU runners, CI checks
- `docs/` — status, checklists, ADRs and DDRs
