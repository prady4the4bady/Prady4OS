#!/usr/bin/env python3
"""DDR-1180 sec.3: the icon gate, stdlib only (CI has no Pillow, no SVG renderer).

  icon_check.py <icon-dir>       check every icon; exit 1 on any failure

A MANIFEST line is `<name>` (a 24x24 icon) or `<name> <W> <H>` (a logo asset on
its own viewBox "0 0 W H", DDR-1180 step 4, recorded in assets/brand/README.md).
Every rule below is then stated in that asset's own units: the grid is
[2, W-2] x [2, H-2], ink stays inside [1, W-1] x [1, H-1], and the renders are
at 16/24/32/64 px HEIGHT with the width scaled to match.

Arms (one FAIL line names the arm, the file and the reason):
  A  exists    every MANIFEST name has <name>.svg and every .svg is listed
  B  subset    only the DDR-1180 sec.2.3 elements, path commands and
               attributes; viewBox "0 0 24 24"; stroke-width 2; currentColor
  C  grid      every coordinate (curve control points included) in [2, 22]
  D  renders   at 16/24/32/64 px: ink present, ink inside [1, 23] units, and
               the ink box matches the 24 px box scaled, within 1.5 px
  E  marker    the "original: PRADYOS, DDR-1180" comment is present

The rasteriser marks a pixel when its centre lies within half a stroke of the
outline (stroke) or inside it by even-odd (fill). It proves an icon draws; it
says nothing about how it looks, which is the operator's review.
"""
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

SVG_NS = "{http://www.w3.org/2000/svg}"
MARKER = "original: PRADYOS, DDR-1180"
SIZES = (16, 24, 32, 64)
GRID_LO, GRID_HI = 2.0, 22.0
INK_LO, INK_HI = 1.0, 23.0
BOX_TOL_PX = 1.5
CURVE_STEPS = 16

ELEMENTS = {"svg", "path", "circle", "rect", "line", "g"}
PAINT = {"currentColor", "none"}
COMMON_ATTRS = {"fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin"}
ATTRS = {
    "svg": COMMON_ATTRS | {"viewBox", "width", "height"},
    "g": COMMON_ATTRS,
    "path": COMMON_ATTRS | {"d"},
    "circle": COMMON_ATTRS | {"cx", "cy", "r"},
    "rect": COMMON_ATTRS | {"x", "y", "width", "height", "rx", "ry"},
    "line": COMMON_ATTRS | {"x1", "y1", "x2", "y2"},
}
PATH_CMDS = set("MLHVCQZmlhvcqz")
ARGC = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "Q": 4, "Z": 0}

failures = []


def fail(arm, name, why):
    failures.append(f"[icons] FAIL arm {arm} {name}: {why}")


def tag(el):
    return el.tag[len(SVG_NS):] if el.tag.startswith(SVG_NS) else el.tag


def num(s):
    return float(s)


def parse_path(d):
    """Return (subpaths, points). Each subpath is (polyline, closed); points are
    every coordinate the path names, control points included, for arm C."""
    toks = re.findall(r"[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
    for t in toks:
        if t.isalpha() and t not in PATH_CMDS:
            raise ValueError(f"path command {t!r} not in M L H V C Q Z")
    subpaths, points = [], []
    cur, start = (0.0, 0.0), (0.0, 0.0)
    poly, cmd, i = [], None, 0
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if poly:
                    subpaths.append((poly, True))
                poly, cur = [], start
                continue
        elif cmd is None:
            raise ValueError("path data does not start with a command")
        n = ARGC[cmd.upper()]
        args = [num(t) for t in toks[i:i + n]]
        if len(args) != n:
            raise ValueError(f"command {cmd} needs {n} numbers")
        i += n
        rel = cmd.islower()
        ox, oy = cur if rel else (0.0, 0.0)
        c = cmd.upper()
        if c == "M":
            if poly:
                subpaths.append((poly, False))
            cur = (ox + args[0], oy + args[1])
            start, poly = cur, [cur]
            points.append(cur)
            cmd = "l" if rel else "L"   # implicit lineto after a moveto
        elif c == "L":
            cur = (ox + args[0], oy + args[1])
            poly.append(cur)
            points.append(cur)
        elif c == "H":
            cur = ((cur[0] if rel else 0.0) + args[0], cur[1])
            poly.append(cur)
            points.append(cur)
        elif c == "V":
            cur = (cur[0], (cur[1] if rel else 0.0) + args[0])
            poly.append(cur)
            points.append(cur)
        else:
            pts = [(ox + args[k], oy + args[k + 1]) for k in range(0, n, 2)]
            ctrl, end = [cur] + pts[:-1], pts[-1]
            points.extend(pts)
            for s in range(1, CURVE_STEPS + 1):
                t = s / CURVE_STEPS
                poly.append(bezier(ctrl + [end], t))
            cur = end
    if poly:
        subpaths.append((poly, False))
    return subpaths, points


def bezier(p, t):
    while len(p) > 1:
        p = [((1 - t) * a[0] + t * b[0], (1 - t) * a[1] + t * b[1]) for a, b in zip(p, p[1:])]
    return p[0]


def shape(el, name):
    """Return (subpaths, points) for one drawing element."""
    t, a = tag(el), el.attrib
    if t == "path":
        return parse_path(a.get("d", ""))
    if t == "line":
        p = [(num(a["x1"]), num(a["y1"])), (num(a["x2"]), num(a["y2"]))]
        return [(p, False)], p
    if t == "circle":
        cx, cy, r = num(a["cx"]), num(a["cy"]), num(a["r"])
        ring = [(cx + r * math.cos(2 * math.pi * k / 48), cy + r * math.sin(2 * math.pi * k / 48))
                for k in range(48)]
        return [(ring, True)], [(cx - r, cy - r), (cx + r, cy + r)]
    if t == "rect":
        x, y, w, h = num(a["x"]), num(a["y"]), num(a["width"]), num(a["height"])
        p = [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
        return [(p, True)], p
    return [], []


def paint(el, parent_style):
    style = dict(parent_style)
    for k in ("fill", "stroke", "stroke-width"):
        if k in el.attrib:
            style[k] = el.attrib[k]
    return style


def collect(root, name):
    """Walk the tree checking arm B; return drawable items (subpaths, points, style)."""
    items = []

    def walk(el, style):
        t = tag(el)
        if t not in ELEMENTS:
            fail("B", name, f"element <{t}> is not in the DDR-1180 subset")
            return
        for k, v in el.attrib.items():
            if k not in ATTRS[t]:
                fail("B", name, f"<{t}> attribute {k!r} is not allowed")
            if k in ("fill", "stroke") and v not in PAINT:
                fail("B", name, f"{k}={v!r}: colour must be currentColor or none")
        st = paint(el, style)
        if t not in ("svg", "g"):
            try:
                sub, pts = shape(el, name)
            except (KeyError, ValueError) as e:
                fail("B", name, f"<{t}> unparseable: {e}")
                return
            items.append((sub, pts, st))
        for ch in el:
            walk(ch, st)

    walk(root, {"fill": "none", "stroke": "none", "stroke-width": "1"})
    return items


def render(items, size, w=24.0, h=24.0):
    s = size / h
    cols = int(math.ceil(w * s))
    ink = [[False] * cols for _ in range(size)]
    for sub, _, st in items:
        half = float(st["stroke-width"]) / 2.0
        segs = []
        for poly, closed in sub:
            q = poly + ([poly[0]] if closed and len(poly) > 1 else [])
            segs.extend(zip(q, q[1:]))
        filled = st["fill"] != "none"
        for py in range(size):
            for px in range(cols):
                if ink[py][px]:
                    continue
                x, y = (px + 0.5) / s, (py + 0.5) / s
                if st["stroke"] != "none" and any(seg_dist(x, y, a, b) <= half for a, b in segs):
                    ink[py][px] = True
                elif filled and inside(x, y, sub):
                    ink[py][px] = True
    return ink


def seg_dist(x, y, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((x - a[0]) * dx + (y - a[1]) * dy) / L))
    return math.hypot(x - (a[0] + t * dx), y - (a[1] + t * dy))


def inside(x, y, sub):
    c = False
    for poly, _ in sub:
        for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
            if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                c = not c
    return c


def ink_box(ink):
    ys = [y for y, row in enumerate(ink) if any(row)]
    xs = [x for row in ink for x, v in enumerate(row) if v]
    if not ys:
        return None
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1)


def check(path, name, w=24, h=24):
    raw = open(path, encoding="utf-8").read()
    if MARKER not in raw:
        fail("E", name, f"missing the '<!-- {MARKER} -->' comment")
    if re.search(r"<!(DOCTYPE|ENTITY)", raw, re.I):
        # The subset has no use for a DTD; refusing one also keeps expat's
        # entity expansion (billion laughs) out of a stdlib-only tool.
        fail("B", name, "DOCTYPE/ENTITY declarations are not allowed")
        return
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as e:
        fail("B", name, f"not well-formed XML: {e}")
        return
    if tag(root) != "svg":
        fail("B", name, "root element is not <svg>")
        return
    want = ["0", "0", str(w), str(h)]
    if root.attrib.get("viewBox", "").split() != want:
        fail("B", name, f"viewBox must be '{' '.join(want)}'")
    items = collect(root, name)
    if not items:
        fail("B", name, "no drawing elements")
        return
    for _, _, st in items:
        if st["stroke"] != "none" and st["stroke-width"] != "2":
            fail("B", name, f"stroke-width {st['stroke-width']!r}, must be 2")
            break
    for _, pts, _ in items:
        for x, y in pts:
            if not (GRID_LO <= x <= w - GRID_LO and GRID_LO <= y <= h - GRID_LO):
                fail("C", name, f"coordinate ({x:g}, {y:g}) outside [2, {w - 2:g}] x [2, {h - 2:g}]")
                break
    boxes = {}
    for size in SIZES:
        box = ink_box(render(items, size, w, h))
        if box is None:
            fail("D", name, f"renders empty at {size} px")
            return
        s = size / float(h)
        lo_x, lo_y, hi_x, hi_y = INK_LO * s, INK_LO * s, (w - INK_LO) * s, (h - INK_LO) * s
        if box[0] < lo_x - 1 or box[1] < lo_y - 1 or box[2] > hi_x + 1 or box[3] > hi_y + 1:
            fail("D", name, f"ink box {box} at {size} px leaves [1, {w - 1:g}] x [1, {h - 1:g}] units")
        boxes[size] = box
    ref = boxes[24]
    for size, box in boxes.items():
        k = size / 24.0
        # One pixel of the 24 px reference is h/24 units, so its rounding error
        # grows with the viewBox height; a 24x24 icon keeps exactly 1.5 px.
        if any(abs(box[i] - ref[i] * k) > BOX_TOL_PX * max(1.0, h / 24.0) for i in range(4)):
            fail("D", name, f"ink box {box} at {size} px does not scale from {ref} at 24 px")


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    d = argv[1]
    man_path = os.path.join(d, "MANIFEST")
    if not os.path.isfile(man_path):
        print(f"[icons] FAIL arm A: no {man_path}")
        return 1
    dims = {}
    for ln in open(man_path, encoding="utf-8"):
        f = ln.split("#", 1)[0].split()
        if not f:
            continue
        if len(f) == 1:
            dims[f[0]] = (24, 24)
        elif len(f) == 3 and f[1].isdigit() and f[2].isdigit() and int(f[1]) > 4 and int(f[2]) > 4:
            dims[f[0]] = (int(f[1]), int(f[2]))
        else:
            fail("A", f[0], f"MANIFEST line {ln.strip()!r}: expected '<name>' or '<name> <W> <H>'")
    names = list(dims)
    files = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".svg"))
    if not names:
        fail("A", "MANIFEST", "lists no icons")
    for n in names:
        if n not in files:
            fail("A", n, "listed in MANIFEST but has no .svg")
    for f in files:
        if f not in names:
            fail("A", f, ".svg present but not listed in MANIFEST")
    for n in names:
        if n in files:
            check(os.path.join(d, n + ".svg"), n, *dims[n])
    for line in failures:
        print(line)
    if failures:
        return 1
    print(f"[icons] OK — {d}: {len(names)} asset(s), arms A-E, sizes {'/'.join(map(str, SIZES))} px")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
