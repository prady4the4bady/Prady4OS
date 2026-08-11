= DDR-907 — item 48 BIOS arm: correcting DDR-905, and reverting its fix

**Status:** DDR-905's conclusion is **WRONG** and is corrected here. The CHS
fallback it specified was implemented, measured, and **reverted**. Item 48
remains PARTIAL.
**Date:** 2026-08-11
**Corrects:** DDR-905. **Follows:** DDR-903.

## What DDR-905 claimed

That SeaBIOS's El Torito hard-disk emulation engaged (`DL=80`) but lacked EDD
(`AH=01` from `AH=42h`), so a CHS `AH=02h` fallback would fix the BIOS arm.

The first half of that is an over-reading of one byte, and the second half
follows from it, so both are wrong.

## The measurement that refutes it

The fallback was built exactly as specified — geometry from `AH=08h`, never
hardcoded — then instrumented and run against the real ISO:

```
PRADYOS S1: loading stage2...
s01h02
R01
PRADYOS S1: DISK READ ERROR
```

| Output | Meaning |
|---|---|
| `s01` | `AH=08h` reports **1 sector per track** |
| `h02` | `AH=08h` reports **2 heads** |
| `R01` | `AH=02h` returns **`AH=01`, invalid command** — same as `AH=42h` |

**`AH=02h` fails identically to `AH=42h`.** The drive rejects *every* read
function, and reports a geometry (1 spt) that no disk has ever had.

## The corrected conclusion

A drive that answers every INT 13h read with "invalid command" and reports
impossible geometry is not a drive missing one optional feature. This is
consistent with **drive 0x80 not being present at all**.

`DL=0x80` never proved hard-disk emulation engaged. `DL` is simply the byte
SeaBIOS left in the register; it carries no evidence about whether the drive
behind it exists. DDR-905 treated one register value as proof of a whole
subsystem's state, and every inference after it inherited that error.

**Why the CHS fallback could never have worked:** it changes *which read
function* stage1 calls. The defect is that there is nothing to call it against.
No choice of read function fixes an absent drive.

## What was reverted, and why

`boot/mbr/boot.asm` and `boot/stage2/stage2.asm` are back at their committed
state. The fallback assembled clean under `-Werror`, fit the 512-byte budget
with 111 bytes to spare, and left the partition table region clear — it was
*correct code*. It just does not address the actual defect.

Keeping it would have put unproven, never-exercised code on the path every BIOS
boot gate depends on, to fix a cause now known to be misdiagnosed. That is dead
code on the most critical path in the system.

## An honest note on process

This is the second time in this project that a fix was designed off an
unverified inference rather than a measurement — the first cost four attempts on
item 16 (DDR-904). The pattern is identical: a plausible reading of partial
evidence was promoted to a premise, and the next several hours were spent
building correctly on top of it.

The cheap check both times was the same: **verify the thing you assumed is
true, before building on it.** One extra probe of whether drive 0x80 answered
*any* function would have cost minutes and saved this slice.

## What is actually still unknown

Why hard-disk emulation does not present a usable drive. Not guessed at here.
The next measurement — not a fix — should establish whether SeaBIOS registered
the El Torito boot image as a drive at all (its own boot-menu/debug output is
the cheapest source).

Candidate directions, none attempted and none to be built before that
measurement lands:

1. **No-emulation boot.** DL becomes the CD drive; EDD reads work but with
   2048-byte sectors, so stage1/stage2 sector arithmetic would need a scale
   factor. This is the conventional approach and is probably where this ends up.
2. **isohybrid MBR** — needs syslinux, not installed.

## Status

Item 48 stays **PARTIAL**: UEFI arm proven (DDR-903), BIOS arm blocked.
`smoke-iso-x86` stays **excluded**. `tools/build/mk_hdimg.py` and the `iso`
target are unchanged and still correct for the UEFI arm.
