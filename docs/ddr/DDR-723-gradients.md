# DDR-723 — multi-stop gradient backdrops

> DDR before code (committed together). The base fill under every ambiance was
> a FLAT `g_bg` rect; the brief's backdrops are gradients.

## Decisions
- **D1 — 3-stop vertical gradient derived from the ambiance bg** (so the
  OKLab ambiance transitions keep working unmodified): stop 0.0 = bg,
  stop 0.35 = bg×1.25 (horizon lightening), stop 1.0 = bg×0.55 (floor
  darkening), clamped; per-row color, one `fill_rect` row each. The per-
  ambiance glows/nebulas (DDR-716) draw over it as before.
- **D2 — sentinel `PRADYOS_GRADIENT_OK`** on the first gradient frame.

## Gate
`smoke-gradient`: GPU boot; asserts `PRADYOS_GRADIENT_OK` +
`PRADYOS_BACKDROP_OK`. 68 gates.

## Non-goals
Arbitrary stop tables per ambiance; dithering; the Inter typeface and other
remaining deferred visuals.
