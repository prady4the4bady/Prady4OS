# assets/brand — PRADYOS brand sources (DDR-1180)

Hand-written SVG only. Every file carries `<!-- original: PRADYOS, DDR-1180 -->`
and was drawn for this project on the grid below: nothing is copied, traced or
converted from an existing OS, icon pack, font or screenshot (DDR-1180 §2.4).
Exports (PNG, atlases) are build outputs under `build/`, never committed.
`make ci-icon-check` checks both directories.

| Directory | Asset | viewBox | Grid (stroke centreline) |
|---|---|---|---|
| `icons/` | every icon | `0 0 24 24` | [2, 22] × [2, 22] |
| `logo/` | `emblem` | `0 0 48 48` | [2, 46] × [2, 46] |
| `logo/` | `wordmark` | `0 0 84 14` | [2, 82] × [2, 12] |

All strokes are 2 units with round caps and joins, painted in `currentColor`.

**Wordmark letter grid.** Capitals with a cap height of 10 (y 2..12), letter
width 8, pitch 12 (a 4-unit gap), the first letter at x = 2. Bowls (P, R, D, O,
S) are quadratic curves on that grid; no font outlines are used (DDR-1180 B2).

**Emblem.** A scorpion seen from above (DDR-1180 B1(b)): pincers forward, six
legs, the tail curling from the rear with its sting turned back over the body.
