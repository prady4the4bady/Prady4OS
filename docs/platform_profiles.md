# PRADYOS — Target ISAs, Platform Profiles & Branch Strategy

The eight logical layers are identical across every target; only the
architecture-specific code under `arch/<isa>/...` and `drivers/.../platform/...`
differs. No giant `#ifdef` jungles — platform divergence lives in per-ISA files.

## Layers (same on every variant)

| Layer | Subsystem | Status (x86_64 reference) |
|-------|-----------|---------------------------|
| L1 | Bootloader (MBR now; UEFI later) | 🟢 MBR two-stage; UEFI deferred |
| L2 | NEXUS kernel core | 🟢 complete; user W^X/NX live (ADR-021). APIC, 3-lane NAS, PFO, AVAS, Sovereign Pool, kernel-self W^X deferred |
| L3 | Drivers | 🟡 ACPI/PCIe/virtio-blk + RTC done; virtio-net, NVMe, framebuffer, USB, power deferred |
| L4 | VFS + filesystems | 🟢 complete — FAT32 read-write, SFS engine (CoW B+tree, journal, snapshots, LZ4), ext4 read-only |
| L5 | Userspace (pradyos-init, PRISM shell, musl, prad) | 🟡 5a static ELF loader + per-process AS + W^X done; syscalls (5b), musl, init, shell, prad pending |
| L6 | AETHER runtime (aetherd, Ollama bridge, cloud adapters, 8 named agents, Sovereign Gate, approval queue) | 🔴 |
| L7 | Desktop / compositor (Wayland + Sovereign/Manual UI) | 🔴 |
| L8 | Quantum Abstraction Layer (`/dev/qpu0`, QAOA, Grover) | 🔴 future |

## Target ISAs & platform profiles

- **x86_64** — Intel x86_64, AMD x86_64. *Reference architecture* (this repo's
  `main`). Always buildable, always passes CI.
- **ARM64**
  - **Generic / SBSA** — server-class ARMv8/v9 (SBSA boot, GIC, generic timer).
  - **Apple Silicon** — M-series (custom boot, AIC, Apple-specific MMIO).
  - **Grace Blackwell (DGX Spark / GB10)** — 20-core Armv9.2 CPU + integrated
    Blackwell GPU + 128 GB unified LPDDR5x (UMA). UMA changes the memory model
    (CPU/GPU shared address space) — relevant to the Sovereign Memory Pool (L2)
    and AETHER weight residency (L6).
- **RISC-V64** — future / experimental.

## Branch strategy

| Branch | Target | Notes |
|--------|--------|-------|
| `main` | x86_64 reference | always passes CI (toolchain, `make image`, `make smoke`) |
| `dev/phase1` | active x86_64 development | fast-forwarded into `main` per slice |
| `feature/arm64` | generic ARM64 (SBSA) | rebases from `main` regularly |
| `feature/arm64-grace` | DGX Spark (Grace Blackwell UMA) | rebases from `main` |
| `feature/apple` | Apple Silicon (M-series) | rebases from `main` |
| `feature/rv64` | RISC-V experiments | rebases from `main` |

Rules:
- `main` always passes CI.
- Variant branches regularly rebase from `main` to inherit reference progress.
- Architecture-specific code goes under `arch/<isa>/…` and
  `drivers/.../platform/...`; shared logic stays ISA-neutral. No `#ifdef`
  jungles — select per-ISA implementations at the build/file level.

Variant branches are created as markers from the current `main` and stay in
lockstep with the reference until per-ISA bring-up begins on each.
