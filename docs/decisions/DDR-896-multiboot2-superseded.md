= DDR-896 — Multiboot2 is superseded by the hybrid BIOS+UEFI ISO (item 48)

**Status:** Accepted — **substitution approved by the owner**, recorded here as a
formal supersession rather than a scope cut
**Date:** 2026-08-10
**Scope:** item 48's specification text. No code in this DDR.
**Supersedes:** the Multiboot2 clause of item 48.

## The decision

Item 48's text names "multiboot2 header + GRUB config". **Multiboot2 is not
implemented.** In its place, the hybrid ISO carries the two boot paths this
project has already built and proven independently:

| Arm | Artifact | Proven by |
|---|---|---|
| BIOS | `build/pradyos.img` — existing MBR/stage1/stage2 chain | every legacy gate since Phase 1 |
| UEFI | `build/esp.img` — `EFI/BOOT/BOOTX64.EFI` + `KERNEL.BIN` | `smoke-uefi`, DDR-886 |

## Why Multiboot2 is the wrong addition here

DDR-886 §1 fixed one handoff contract:

```
RDI = physical &boot_info (0x4000); kernel.bin at 0x400000;
0xFFFFFFFF80000000 -> 0x400000 plus a low identity map; long mode; IF=0
```

Two independent loaders already implement it — stage2 in 16-bit asm, and the
UEFI application in C. Both are gated on the **same** `NEXUS KERNEL OK` sentinel,
which is what proves they agree.

Multiboot2 hands control in **32-bit protected mode** with its own information
structure. Adopting it means a **third** implementation: a 32-bit entry stub that
re-establishes long mode, rebuilds the higher-half mapping, translates the
Multiboot2 tag list into `struct boot_info`, and jumps. That is not a
configuration change; it is another bootloader to keep byte-compatible with the
other two.

DDR-886 §1 states the cost directly: *a kernel that boots one way and not the
other fails far from the loader that caused it.* Three contracts make that
failure mode three-way.

Multiboot2 buys the ability for a general-purpose bootloader to load an arbitrary
kernel. PRADYOS does not need that: it ships its own loaders, and the ISO's job
is to carry them, not to replace them.

## What item 48 now requires

1. A hybrid El Torito image with **two catalog entries** — a BIOS entry booting
   the existing image, and an EFI entry (platform id 0xEF) pointing at the
   existing ESP.
2. `make iso`.
3. `smoke-iso-x86` booting the **actual ISO** twice — once via SeaBIOS, once via
   OVMF — each requiring `NEXUS KERNEL OK`, plus `[uefi] handoff` on the UEFI arm
   only.

The item's intent — one artifact that boots both paths, proven by gate — is
satisfied in full. Only the Multiboot2 mechanism is dropped, and it is dropped
**explicitly**, here, with the reason.

## Tracker obligation

`docs/build_status.md` and `BUILD_TRACKER.md` must record item 48 as
*"Multiboot2 superseded per DDR-896 (owner-approved); hybrid BIOS+UEFI El Torito
instead"* — not as an unqualified completion, and not silently.
