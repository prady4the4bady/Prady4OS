= DDR-903 — hybrid ISO: UEFI arm boots, BIOS arm blocked on El Torito + EDD

**Status:** Item 48 **PARTIAL**. ISO builds; UEFI arm proven; BIOS arm open.
Gate registered **EXCLUDED** until both arms pass.
**Date:** 2026-08-11
**Implements:** DDR-896 (Multiboot2 superseded, owner-approved).

## What works

`make iso` produces a 52.8 MB hybrid El Torito image carrying both loaders — no
Multiboot2, no third handoff contract, exactly as DDR-896 specifies.

**The UEFI arm boots the real kernel from the real ISO:**

```
[uefi] PRADYOS loader
[uefi] handoff
NEXUS KERNEL OK
```

Same `NEXUS KERNEL OK` sentinel as every other boot path, from an artifact
produced by `xorriso`, booted under OVMF. That half of item 48 is genuinely done.

## What does not work, and precisely why

The BIOS arm reaches stage1 and dies in its first disk read:

```
PRADYOS S1: loading stage2...
PRADYOS S1: DISK READ ERROR
```

stage1 is not at fault. It takes its drive number from the `DL` the BIOS hands
it (`boot.asm:40`), and its DAP is an ordinary 16-sector read from LBA 1 to
`0000:7E00`. The problem is the container.

**All three El Torito emulations were considered; two were measured:**

| Emulation | Result |
|---|---|
| **floppy, 2.88 MiB** | **MEASURED FAIL** — right geometry (512-byte sectors) but emulated floppies do not implement `INT 13h AH=42h`, which is exactly the extended read stage1 issues |
| **no-emulation** | rejected by analysis — the BIOS presents 2048-byte CD sectors, so every 512-byte LBA in stage1/stage2 lands four times too deep |
| **hard disk** | **MEASURED FAIL** — drive 0x80 with EDD is what stage1 needs, and `tools/build/mk_hdimg.py` adds the required MBR partition table (the 0x1BE..0x1FD region in `stage1.bin` was verified all-zero first), but the read still fails |

The hard-disk result is the surprising one and is **not yet explained**. It is
recorded as an open question rather than guessed at, consistent with how the
item-16 hypotheses were handled.

## Candidate next steps, none attempted

1. **Print `DL` from stage1 under the ISO.** One byte of output settles whether
   SeaBIOS actually engaged hard-disk emulation or handed over the raw CD drive.
   That is the measurement to take first.
2. **Add a CHS (`AH=02h`) fallback to stage1** when `AH=42h` fails. This makes
   the boot chain work under floppy emulation too, but it modifies the loader
   every existing BIOS gate depends on — so it needs its own regression pass, not
   a drive-by edit.
3. **isohybrid MBR.** Would make the ISO bootable as a raw disk image, but the
   template ships with syslinux, which is not installed.

## Gate status

`smoke-iso-x86` exists and tests **both** arms. It is registered **excluded** in
`gate_shards.txt`: a gate that cannot pass must not enter the CI matrix, and
weakening it to UEFI-only would let the BIOS arm rot while the suite looked
green — the precise failure DDR-886 warned about when it insisted both loaders
share one sentinel.

**Item 48 is PARTIAL and is recorded as such.** The ISO exists and boots on UEFI;
it does not yet boot on BIOS.
