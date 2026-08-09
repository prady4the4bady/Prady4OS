= DDR-887 — item 48 (ISO pipeline) is blocked on build-host tooling

**Status:** Blocked — design recorded, not implemented
**Date:** 2026-08-10
**Scope:** none yet. `.github/workflows/ci.yml` dependency list only.

## The blocker, stated exactly

Item 48 needs `grub-mkrescue` (or `xorriso` directly) plus GRUB's BIOS and EFI
module trees to produce a hybrid El Torito image. On this build host:

```
xorriso: MISSING          grub-mkrescue: MISSING
mkisofs: MISSING          genisoimage:   MISSING
grub bios modules: 0      grub efi modules: 0
sudo -n  ->  "sudo: a password is required"
```

The packages are not installed and **I cannot install them** — this session has
no interactive sudo. That is an environment fact, not a design problem.

`Dockerfile` already lists `xorriso`, `grub-pc-bin` and `grub-efi-amd64-bin`
(item 2's reproducible environment), but CI does **not** build from that
Dockerfile — its workflow installs its own package list, which lacked them. That
list is corrected in this commit.

## Why no gate ships anyway

A `make iso` rule could be written blind. It would not be verifiable here: this
project's standard is 20 local runs before a gate is trusted, and a gate that has
never once run locally is exactly the kind of thing that goes red in CI for a
reason nobody can attribute. Writing it now would trade a known blocker for an
unknown one.

## The design, recorded so the next session does not re-derive it

**Hybrid El Torito with two catalog entries** — one per boot path, which is what
item 48's "BOTH the legacy MBR path and the UEFI path" requires:

1. **UEFI arm — already solved.** The El Torito EFI entry (platform id 0xEF)
   points at a FAT image. `build/esp.img` from DDR-886 *is* that image, already
   carrying `EFI/BOOT/BOOTX64.EFI` and `KERNEL.BIN`, and already proven to boot
   the kernel under OVMF. This arm needs no new code.

2. **BIOS arm — floppy emulation is the cheap route.** `build/pradyos.img` is a
   2 MiB bootable image whose stage1 already reads stage2 and the kernel from
   known LBAs. El Torito 2.88 MiB floppy emulation presents it to the BIOS as
   drive 0, so stage1/stage2 run **unmodified**. Padding 2 MiB → 2,949,120 bytes
   is the only change. The alternative — GRUB + a multiboot2 header — is
   strictly more work, because multiboot2 hands control in 32-bit protected mode
   while `kernel_entry` requires long mode with the DDR-886 contract already
   established, so it would mean a third implementation of stage2.

**On multiboot2 specifically:** the item names it, but it buys nothing here. Its
value is letting a general-purpose bootloader load an arbitrary kernel; PRADYOS
already has two loaders that establish its exact contract. Adding a multiboot2
header plus a 32-bit shim would be a third handoff implementation to keep in
sync, and DDR-886 §1 is explicit that divergence between boot paths fails far
from its cause. **Recommendation: floppy-emulation El Torito, and multiboot2
declined with this reasoning** — flagged for sign-off rather than decided
silently, since it deviates from the item text.

**Gate shape:** `smoke-iso-x86` boots the ISO twice — once with SeaBIOS and once
with OVMF — and requires `NEXUS KERNEL OK` from both, plus `[uefi] handoff` from
the UEFI arm only. Two arms, one artifact, same sentinel: that is what proves
one ISO covers both paths.

## To unblock

```
sudo apt-get install -y xorriso grub-pc-bin grub-efi-amd64-bin ovmf
```

`ovmf` is in that list because `smoke-uefi` (DDR-886) needs it too, and CI did
not have it either — see below.

## The CI dependency gap this exposed

`smoke-uefi` shipped in the previous commit and is registered in shard 0. CI's
workflow installed `dosfstools mtools e2fsprogs` and **no `ovmf`**, so that gate
would go red on a missing firmware file rather than on anything about the
kernel. `ovmf xorriso grub-pc-bin grub-efi-amd64-bin` are added to the workflow
here.

The harness already reports this case as a **HOST-ENV** failure with the exact
missing path rather than as a kernel failure — that path was written in DDR-886
precisely so a missing firmware never looks like a boot regression.
