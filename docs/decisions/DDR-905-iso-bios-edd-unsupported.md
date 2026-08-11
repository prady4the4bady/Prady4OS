= DDR-905 — item 48 BIOS arm: the emulated drive has no EDD

**Status:** Root cause ESTABLISHED by measurement. Fix scoped, not yet written.
**Date:** 2026-08-11
**Follows:** DDR-903.

## The measurement

Two bytes settled it. stage1 was temporarily instrumented to print the BIOS boot
drive and the `INT 13h` status, then reverted.

```
PRADYOS S1: loading stage2...
S1 DL=80
S1 AH=01
PRADYOS S1: DISK READ ERROR
```

**`DL=80`** — SeaBIOS *did* engage El Torito hard-disk emulation. The boot
catalog is correct and the xorriso invocation is correct.

**`AH=01` — "invalid command".** The emulated drive does not implement
`INT 13h AH=42h`, the EDD extended read that stage1 issues.

## What this rules out

The partition table is **not** the fault, and neither is any LBA-offset
mismatch. Those were the leading hypotheses and both are eliminated: a
partition-relative versus absolute addressing error would return wrong *data* or
a sector-not-found (`AH=04`), never "invalid command". `AH=01` means the BIOS
rejected the *function*, before any address was interpreted.

`tools/build/mk_hdimg.py` and its partition entry are therefore correct as
written and stay.

## The actual fix, and its true scope

Add a **CHS fallback** (`INT 13h AH=02h`) wherever an EDD read is issued, taken
when `AH=42h` returns `CF=1` with `AH=01`.

Two call sites, not one:

| File | Line | Note |
|---|---|---|
| `boot/mbr/boot.asm` | 47 | inside the **512-byte** boot sector — the binding constraint |
| `boot/stage2/stage2.asm` | 187 | more room available |

The fallback needs the drive geometry (`AH=08h` returns sectors-per-track in
`CL[5:0]` and max head in `DH`) and an LBA→CHS conversion. Hard-coding a
geometry would be the same class of error as the DDR-895 clamp constant.

**This touches the loader every existing BIOS gate depends on.** It is a slice
with its own regression pass, not a drive-by edit, which is why it is scoped here
rather than attempted at the end of a session.

## Why the UEFI arm is unaffected

The UEFI loader reads through `EFI_FILE_PROTOCOL`, not `INT 13h`. It already
boots the same ISO to `NEXUS KERNEL OK` (DDR-903). Only the BIOS arm is blocked,
and only by this one missing BIOS function.

## Status

Item 48 remains **PARTIAL**. `smoke-iso-x86` stays **excluded** until both arms
boot. The remaining work is now a specified change with a known cause, rather
than an open question.
