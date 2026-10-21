#!/usr/bin/env python3
"""tools/kreloc/kreloc.py -- the KASLR relocation table (DDR-1179 sec.2.3, Amendment 1).

Reads a kernel.elf linked with --emit-relocs and writes the table that the boot
applier (DDR-1179 sec.5 step 3) walks to add the slide to every absolute field.

    kreloc.py <kernel.elf> <out.bin> --reserve <bytes> --expect <n>
    kreloc.py --verify <kernel.elf>       (decodes the table IN the .kreloc section)
    kreloc.py --slide-check <kernel.elf> <kernel.bin> <slid.bin> <slide>
        applies the table in kernel.bin exactly as boot.asm does, with <slide>,
        and requires the result to equal slid.bin -- the same objects linked
        <slide> bytes higher (DDR-1179 A2). A field the table misses, or a
        decoding slip, leaves a difference.

Rules (each failure exits non-zero, so the build fails):
  * PC-relative types (PC32, PLT32) need no patching and are skipped;
  * R_X86_64_64 and R_X86_64_32S are recorded when the value the linker wrote
    into the field lies inside the image [__kimg_lo, __bss_end] -- a field that
    holds an absolute constant (a SHN_ABS symbol) must NOT move, and is counted;
  * any other relocation type is a design change: the build fails;
  * the encoded table must fit the .kreloc reservation.

Format (little-endian): b"KRL1", u32 count, u32 payload bytes, then `count`
LEB128 varints of ((offset - previous_offset) << 1 | is64), offsets from the
link base, strictly increasing. Zero padding up to the reservation.
"""
import struct
import sys

R_NONE, R_64, R_PC32, R_PLT32, R_32S = 0, 1, 2, 4, 11
SKIP = {R_NONE, R_PC32, R_PLT32}
SHT_SYMTAB, SHT_RELA, SHF_ALLOC = 2, 4, 0x2
MAGIC = b"KRL1"


def die(msg):
    print("kreloc: FAIL - " + msg, file=sys.stderr)
    sys.exit(1)


class Elf:
    def __init__(self, path):
        self.b = open(path, "rb").read()
        b = self.b
        if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
            die("%s: not a little-endian ELF64" % path)
        if struct.unpack_from("<H", b, 0x10)[0] != 2:
            die("%s: not ET_EXEC (r_offset would not be a VA)" % path)
        (self.shoff,) = struct.unpack_from("<Q", b, 0x28)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from("<HHH", b, 0x3A)
        self.sh = []
        for i in range(self.shnum):
            f = struct.unpack_from("<IIQQQQIIQQ", b, self.shoff + i * self.shentsize)
            self.sh.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3], off=f[4],
                                size=f[5], link=f[6], info=f[7], entsize=f[9]))
        strtab = self.sh[self.shstrndx]
        for s in self.sh:
            n = strtab["off"] + s["name"]
            s["sname"] = b[n:b.index(b"\0", n)].decode()

    def section(self, name):
        for s in self.sh:
            if s["sname"] == name:
                return s
        return None

    def symbol(self, name):
        for s in self.sh:
            if s["type"] != SHT_SYMTAB:
                continue
            strs = self.sh[s["link"]]
            for k in range(s["size"] // 24):
                st_name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", self.b, s["off"] + k * 24)
                n = strs["off"] + st_name
                if self.b[n:self.b.index(b"\0", n)].decode() == name:
                    return value
        die("symbol %s not found" % name)

    def field(self, va, width):
        """The bytes the linker wrote at VA (from the allocated section that holds it)."""
        for s in self.sh:
            if s["flags"] & SHF_ALLOC and s["type"] != 8 and s["addr"] <= va < s["addr"] + s["size"]:
                o = s["off"] + (va - s["addr"])
                return struct.unpack_from("<Q" if width == 8 else "<i", self.b, o)[0]
        die("field at %#x is in no loaded section" % va)


def collect(elf):
    lo, hi = elf.symbol("__kimg_lo"), elf.symbol("__bss_end")
    kreloc = elf.section(".kreloc")
    if kreloc is None:
        die("no .kreloc section (kernel/kernel.ld)")
    out, counts, absolute = {}, {R_64: 0, R_32S: 0}, 0
    for s in elf.sh:
        if s["type"] != SHT_RELA:
            continue
        tgt = elf.sh[s["info"]]
        if not tgt["flags"] & SHF_ALLOC:
            continue                       # debug/notes: never loaded
        for k in range(s["size"] // 24):
            r_off, r_info, _ = struct.unpack_from("<QQq", elf.b, s["off"] + k * 24)
            typ = r_info & 0xFFFFFFFF
            if typ in SKIP:
                continue
            if typ not in (R_64, R_32S):
                die("relocation type %d at %#x (in %s): not one of PC32/PLT32/32S/64 "
                    "(DDR-1179 sec.2.3: an unknown type is a design change)" % (typ, r_off, tgt["sname"]))
            va = r_off                     # ET_EXEC: r_offset is the field's VA
            width = 8 if typ == R_64 else 4
            val = elf.field(va, width)
            if typ == R_32S:
                val &= 0xFFFFFFFFFFFFFFFF  # sign-extended into the top 2 GiB
            if not lo <= val <= hi:
                absolute += 1              # a constant: it must not slide
                continue
            if kreloc["addr"] <= va < kreloc["addr"] + kreloc["size"]:
                die("a relocated field inside .kreloc itself")
            off = va - lo + 2              # MUTANT
            if off in out and out[off] != (typ == R_64):
                die("two widths at offset %#x" % off)
            out[off] = typ == R_64
            counts[typ] += 1
    return sorted(out.items()), counts, absolute, kreloc


def encode(entries):
    body, prev = bytearray(), -1
    for off, is64 in entries:
        v = ((off - prev) << 1) | int(is64)
        prev = off
        while True:
            byte = v & 0x7F
            v >>= 7
            body.append(byte | (0x80 if v else 0))
            if not v:
                break
    return MAGIC + struct.pack("<II", len(entries), len(body)) + bytes(body)


def decode(blob):
    if blob[:4] != MAGIC:
        die("table magic")
    count, n = struct.unpack_from("<II", blob, 4)
    p, prev, out = 12, -1, []
    for _ in range(count):
        v = shift = 0
        while True:
            byte = blob[p]
            p += 1
            v |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        prev += v >> 1
        out.append((prev, bool(v & 1)))
    if p != 12 + n:
        die("table length %d != header %d" % (p - 12, n))
    return out


def apply_slide(img, tab_off, slide):
    """boot.asm's .reloc_* loop, step for step, on a flat image (file offset =
    VA - link base)."""
    p = tab_off
    if img[p:p + 4] != MAGIC:
        die("slide-check: no table magic in the image")
    count = struct.unpack_from("<I", img, p + 4)[0]
    p, off = p + 12, -1
    for _ in range(count):
        v = shift = 0
        while True:
            byte = img[p]
            p += 1
            v |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        off += v >> 1
        if v & 1:
            struct.pack_into("<Q", img, off, (struct.unpack_from("<Q", img, off)[0] + slide) & (2**64 - 1))
        else:
            struct.pack_into("<I", img, off, (struct.unpack_from("<I", img, off)[0] + slide) & 0xFFFFFFFF)


def main(argv):
    if len(argv) == 6 and argv[1] == "--slide-check":
        elf = Elf(argv[2])
        img = bytearray(open(argv[3], "rb").read())
        ref = open(argv[4], "rb").read()
        slide = int(argv[5], 0)
        if len(img) != len(ref):
            die("slide-check: %d B vs %d B" % (len(img), len(ref)))
        # By symbol: "KRL1" also occurs as boot.asm's compare immediate.
        tab = elf.symbol("kreloc_table") - elf.symbol("__kimg_lo")
        apply_slide(img, tab, slide)
        bad = [i for i in range(len(ref)) if img[i] != ref[i]]
        if bad:
            die("slide-check: relocated image differs from the %#x-higher link at %d byte(s), first at "
                "image offset %#x" % (slide, len(bad), bad[0]))
        print("kreloc: slide-check OK (+%#x: relocated image == relinked image, %d B)" % (slide, len(img)))
        return
    if len(argv) == 3 and argv[1] == "--verify":
        elf = Elf(argv[2])
        entries, _, _, kreloc = collect(elf)
        if decode(elf.b[kreloc["off"]:kreloc["off"] + kreloc["size"]]) != entries:
            die("the .kreloc section of %s does not decode to the link's relocations" % argv[2])
        print("kreloc: verify OK (%d entries in .kreloc)" % len(entries))
        return
    if len(argv) != 7 or argv[3] != "--reserve" or argv[5] != "--expect":
        die("usage: kreloc.py <kernel.elf> <out.bin> --reserve <bytes> --expect <n> | --verify <elf>")
    elf, reserve, expect = Elf(argv[1]), int(argv[4]), int(argv[6])
    entries, counts, absolute, kreloc = collect(elf)
    if len(entries) + absolute != expect:
        die("%d recorded + %d absolute != %d R_X86_64_64/32S relocations counted by llvm-readelf"
            % (len(entries), absolute, expect))
    blob = encode(entries)
    if kreloc["size"] != reserve:
        die(".kreloc is %d B in the link, the Makefile reserves %d B" % (kreloc["size"], reserve))
    if len(blob) > reserve:
        die("table is %d B, over the %d B .kreloc reservation" % (len(blob), reserve))
    if decode(blob) != entries:
        die("encoder round trip")
    open(argv[2], "wb").write(blob + bytes(reserve - len(blob)))
    print("kreloc: 64=%d 32S=%d absolute_skipped=%d entries=%d table=%d B reserve=%d B"
          % (counts[R_64], counts[R_32S], absolute, len(entries), len(blob), reserve))


if __name__ == "__main__":
    main(sys.argv)
