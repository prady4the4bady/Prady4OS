= DDR-908 — item 48: the BIOS arm boots. Two stacked defects, both measured.

**Status:** BIOS arm reaches `NEXUS KERNEL OK`. Supersedes DDR-907's conclusion
and vindicates DDR-905's.
**Date:** 2026-08-11
**Closes:** DDR-887, DDR-903, DDR-905, DDR-907.

## The measurement that broke the deadlock

`INT 13h AH=15h` (Get Drive Type) predates EDD and answers presence without
depending on any read succeeding:

```
D00=C01     drive 0x00: CF set, error 01 — no floppy
D80=03      drive 0x80: TYPE 03 — a hard disk IS present
DE0=01      drive 0xE0: type 01
```

**Drive 0x80 exists and is a hard disk.** DDR-907's "no drive present" is
refuted. That was the second wrong inference on this item, and both had the same
shape: a single value read as proof of a whole subsystem's state.

## Defect 1 — the partition end CHS *is* the geometry

The boot catalog was correct all along:

```
El Torito boot img : 1  BIOS  y  hd  0x0000  0x0c  1  24610
```

Hard-disk emulation was requested and honoured. The fault was one field in the
MBR that `mk_hdimg.py` wrote:

```
MBR endCHS h=1 s=1 c=4
```

SeaBIOS **derives the emulated drive's geometry from the partition entry's end
CHS**: `heads = end_head + 1`, `spt = end_sector`. From `h=1 s=1` it computed a
**2-head, 1-sector-per-track** disk — which is exactly what stage1 then measured
back:

```
s01h02
```

`chs(total - 1)` was arithmetically correct for LBA 4095 under 16H/63S. But that
value only round-trips as a geometry description if the partition ends on a
**cylinder boundary**. It did not, so the BIOS invented a degenerate disk on
which every LBA was out of range — and returned `AH=01` for `AH=42h` *and*
`AH=02h` alike. That identical failure is what made the drive look absent.

**Fix:** truncate the partition to whole cylinders and write the end as the last
sector of the last head of the last cylinder — the only form that reproduces
HEADS/SPT when read back.

```
hdimg: 4096 sectors, partition LBA 1..4031, geometry 16H/63S/4C
s3F h0F     <- 63 sectors/track, 16 heads. Correct.
```

## Defect 2 — there really is no EDD

With sane geometry, `AH=42h` **still** returned `01`:

```
s3Fh0F
E01
```

So DDR-905's original diagnosis was right: the El Torito emulated drive has no
EDD. Its prescribed CHS fallback was never the wrong fix — it was an
**unreachable** one, sitting on top of a geometry defect that broke CHS reads
too. Restoring it on the corrected geometry boots:

```
PRADYOS S1: loading stage2...
PRADYOS S2: protected-mode loader
PRADYOS BOOT OK
PRADYOS S2: enabling long mode, jumping to NEXUS...
NEXUS KERNEL OK
```

## What ships

- `tools/build/mk_hdimg.py` — cylinder-aligned partition; the end-CHS field is
  documented as load-bearing rather than decorative.
- `boot/mbr/boot.asm`, `boot/stage2/stage2.asm` — CHS `AH=02h` fallback taken
  only when `AH=42h` fails. Geometry from `AH=08h`, never hardcoded. One sector
  per call, so no read can straddle a track boundary and there is no track
  arithmetic to get wrong. Refusals rather than silent wrap on spt==0, cylinder
  >= 1024, and LBA above 16 bits.

## The lesson, which is the same one as item 16

A correct fix that cannot execute is indistinguishable from a wrong fix. DDR-905
was accurate and measured zero effect, and that null result was then read as
"the diagnosis is wrong" rather than "something upstream stops this running".
Item 16 (DDR-904) failed the same way through four attempts.

**Check that the path executes before concluding anything from what it does.**

The corollary worth keeping: a null result refutes *the conjunction* of the fix
and its reachability, never the fix alone.

## Regression

Boot loaders sit on every BIOS gate's path, so the full suite was re-run.

A first attempt reported 12x `PASS` with **empty gate names** — a shell variable
that never expanded, so every line ran bare `make`, which trivially succeeds.
Twelve green lines that tested nothing. Discarded, and the harness now aborts on
an empty gate name. Same defect class this project keeps finding: a check that
absorbs invalid input and reports success.
