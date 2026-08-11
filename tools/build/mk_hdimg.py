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


def chs(lba, heads=16, spt=63):
    """Legacy CHS triple, clamped to the classic 1023/15/63 maximum."""
    c = lba // (heads * spt)
    h = (lba // spt) % heads
    s = (lba % spt) + 1
    if c > 1023:
        c, h, s = 1023, heads - 1, spt
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

    # One partition spanning the image from LBA 1. It starts at 1, not 0, so the
    # entry never claims the MBR itself as partition content.
    entry = bytearray(16)
    entry[0] = 0x80                                   # bootable
    entry[1:4] = chs(1)                               # first sector CHS
    entry[4] = 0x0C                                   # FAT32 LBA; type is cosmetic here
    entry[5:8] = chs(total - 1)                       # last sector CHS
    entry[8:12] = (1).to_bytes(4, "little")           # first LBA
    entry[12:16] = (total - 1).to_bytes(4, "little")  # sector count
    data[0x1BE:0x1CE] = entry

    with open(dst, "wb") as f:
        f.write(data)
    print("hdimg: %s -> %s (%d sectors, bootable partition at LBA 1)"
          % (src, dst, total))


if __name__ == "__main__":
    main()
