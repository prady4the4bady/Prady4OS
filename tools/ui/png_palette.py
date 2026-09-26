#!/usr/bin/env python3
"""DDR-1147 U-a: sample colours from the UI reference PNGs, stdlib only.

Decodes 8-bit RGB / RGBA non-interlaced PNGs (zlib + the five PNG row filters;
no Pillow, which is not installed in CI) and prints either:

  sample <png> x,y[,r] ...      mean RGB of a (2r+1)^2 box at each point
  bands  <png> x0,y0,x1,y1 N    N mean colours along a column/row strip
  accent <png> x0,y0,x1,y1       mean of the most chromatic 5% of a box
                                (max(rgb)-min(rgb)); robust to glossy
                                gradients where a single point is not

Coordinates are in image pixels (the references are 1536x1024). The output is
hex RGB; nothing here chooses a palette, it only measures one. What a region
MEANS (panel background, accent, text) is written beside the numbers in the
DDR, by a human reading the image -- the script cannot know it.
"""
import struct, sys, zlib


def decode(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG')
    pos, idat, hdr = 8, [], None
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat.append(body)
        elif t == b'IEND':
            break
        pos += 12 + n
    w, h, bd, ct, _, _, il = hdr
    if bd != 8 or ct not in (2, 6) or il != 0:
        sys.exit(f'{path}: unsupported (bit depth {bd}, colour type {ct}, interlace {il})')
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
            elif f != 0:
                sys.exit(f'{path}: bad filter {f} at row {y}')
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


def mean(img, x0, y0, x1, y1):
    w, h, bpp, px = img
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w - 1, x1), min(h - 1, y1)
    s, n = [0, 0, 0], 0
    for y in range(y0, y1 + 1):
        row = y * w * bpp
        for x in range(x0, x1 + 1):
            o = row + x * bpp
            s[0] += px[o]; s[1] += px[o + 1]; s[2] += px[o + 2]; n += 1
    return '#%02X%02X%02X' % tuple(round(v / n) for v in s)


def accent(img, x0, y0, x1, y1):
    w, h, bpp, px = img
    cs = []
    for y in range(max(0, y0), min(h - 1, y1) + 1):
        row = y * w * bpp
        for x in range(max(0, x0), min(w - 1, x1) + 1):
            o = row + x * bpp
            r, g, b = px[o], px[o + 1], px[o + 2]
            cs.append((max(r, g, b) - min(r, g, b), r, g, b))
    cs.sort(reverse=True)
    top = cs[:max(1, len(cs) // 20)]
    n = len(top)
    return '#%02X%02X%02X' % tuple(round(sum(t[i] for t in top) / n) for i in (1, 2, 3))


def main(argv):
    if len(argv) < 3 or argv[0] not in ('sample', 'bands', 'accent'):
        sys.exit(__doc__)
    img = decode(argv[1])
    if argv[0] == 'sample':
        for spec in argv[2:]:
            v = [int(t) for t in spec.split(',')]
            x, y, r = v[0], v[1], (v[2] if len(v) > 2 else 2)
            print(f'{x},{y} r={r} {mean(img, x - r, y - r, x + r, y + r)}')
    elif argv[0] == 'accent':
        for spec in argv[2:]:
            x0, y0, x1, y1 = (int(t) for t in spec.split(','))
            print(f'{spec} {accent(img, x0, y0, x1, y1)}')
    else:
        x0, y0, x1, y1 = (int(t) for t in argv[2].split(','))
        n = int(argv[3])
        for k in range(n):
            xa = x0 + (x1 - x0) * k // n; xb = x0 + (x1 - x0) * (k + 1) // n
            ya = y0 + (y1 - y0) * k // n; yb = y0 + (y1 - y0) * (k + 1) // n
            print(f'{k} {mean(img, xa, ya, max(xa, xb - 1), max(ya, yb - 1))}')


if __name__ == '__main__':
    main(sys.argv[1:])
