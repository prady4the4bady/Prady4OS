#!/usr/bin/env python3
"""Add an MBR partition table to a copy of the boot image (DDR-896, item 48).

WHY THIS EXISTS

El Torito offers three boot-image emulations, and only one of them works here:

  * no-emulation  — the BIOS presents 2048-byte CD sectors. Every LBA stage1 and
    stage2 use is a 512-byte offset, so each read would land four times too far
    into the image.
  * floppy (2.88 MiB) — presents 512-byte sectors on drive 0x00, which is the
    right geometry, but emulated floppies do NOT support INT 13h AH=42h. stage1
    issues exactly that extended read and gets DISK READ ERROR. Measured, not
    assumed: the BIOS arm printed "PRADYOS S1: DISK READ ERROR".
  * hard disk — drive 0x80, 512-byte sectors, EDD supported. This one. It
    requires the image to carry a valid MBR partition table.

stage1 already takes its drive number from the DL the BIOS hands it, so nothing
in the boot chain changes; only the container does.

The partition entry is written into a COPY. The bytes at 0x1BE..0x1FD in
stage1.bin are all zero, so nothing of stage1 is overwritten — verified before
writing this, not hoped for.
"""
import sys

SECTOR = 512


HEADS = 16
SPT = 63


def chs_bytes(c, h, s):
    """Pack a CHS triple into the 3-byte on-disk form."""
    return bytes([h, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF])


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = bytearray(f.read())

    if len(data) < SECTOR:
        sys.exit("image shorter than one sector")
    if data[0x1FE:0x200] != b"\x55\xAA":
        sys.exit("source has no 0x55AA boot signature — wrong file?")

    # Refuse rather than silently clobber: if stage1 ever grows into the table
    # region, overwriting it would corrupt the boot sector in a way that only
    # shows up as a mystery hang on the BIOS arm.
    if any(data[0x1BE:0x1FE]):
        sys.exit("0x1BE..0x1FD is not empty — stage1 grew into the partition table")

    total = (len(data) + SECTOR - 1) // SECTOR

    # DDR-908: the END CHS field is not decoration — SeaBIOS's El Torito
    # hard-disk emulation DERIVES the emulated drive's geometry from it, as
    # heads = end_head + 1 and sectors-per-track = end_sector.
    #
    # That inference only round-trips if the partition ends on a cylinder
    # boundary. The previous version wrote the arithmetically-correct CHS for
    # the last sector of the image (h=1 s=1 c=4 for a 4096-sector image), from
    # which SeaBIOS inferred a 2-head, 1-sector-per-track disk. Every read then
    # fell outside that degenerate geometry and INT 13h returned AH=01 for both
    # AH=42h and AH=02h — measured as "s01h02" from stage1 itself.
    #
    # So the partition is truncated to whole cylinders and the end CHS is
    # written as the last sector of the last head of the last cylinder, which
    # is the only form that reproduces HEADS/SPT when read back.
    cyl = total // (HEADS * SPT)
    if cyl < 1:
        sys.exit("image is smaller than one %dx%d cylinder (%d sectors)"
                 % (HEADS, SPT, total))
    last = cyl * HEADS * SPT - 1          # last LBA inside whole cylinders

    entry = bytearray(16)
    entry[0] = 0x80                                   # bootable
    entry[1:4] = chs_bytes(0, 0, 2)                   # first sector = LBA 1
    entry[4] = 0x0C                                   # FAT32 LBA; cosmetic here
    entry[5:8] = chs_bytes(cyl - 1, HEADS - 1, SPT)   # defines the geometry
    entry[8:12] = (1).to_bytes(4, "little")           # first LBA
    entry[12:16] = last.to_bytes(4, "little")         # sectors: LBA 1..last
    data[0x1BE:0x1CE] = entry

    with open(dst, "wb") as f:
        f.write(data)
    print("hdimg: %s -> %s (%d sectors, partition LBA 1..%d, geometry %dH/%dS/%dC)"
          % (src, dst, total, last, HEADS, SPT, cyl))


if __name__ == "__main__":
    main()
