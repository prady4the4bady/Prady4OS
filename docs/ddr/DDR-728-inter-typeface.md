# DDR-728 — the Inter typeface (bitmap atlas subset)

> DDR before code (committed together). The LAST deferred L7 visual. Inter
> lands as a pre-rendered 16px alpha glyph atlas — not a TTF + rasterizer —
> honoring the brief's look without porting font software into the OS.

## Decisions
- **D1 — host-side generation.** `tools/fontgen/gen_inter.c` (+ vendored
  `stb_truetype.h`, public domain) rasterizes Inter-Regular (SIL OFL 1.1,
  rsms/inter v4.1) at 16 px, ASCII 0x20–0x7E, into `user/inter_font.h`:
  8-bit alpha glyphs + per-glyph metrics, ~22 KB. The GENERATED header is
  committed (reproducible via the tool); the TTF is NOT vendored — rendered
  bitmaps are not font software and the OFL does not restrict rendered output;
  attribution retained in the generated header. The no-out-of-tree-libs wall
  governs the OS image, not build-host tooling (clang/mtools precedent).
- **D2 — alpha-blended rendering.** `draw_str_inter` blends each glyph pixel
  via `blend_px` (a/255), pen-advanced by real glyph metrics — proportional
  text replaces the 8×8 monospace where 16 px fits: window titles first
  (TITLEBAR=18). The 8×8 face stays for small text (agent card labels).
- **D3 — sentinel `PRADYOS_FONT_OK`** on the first Inter render.

## Gate
`smoke-font`: GPU boot; asserts `PRADYOS_FONT_OK` + `PRADYOS_TITLE_OK` (titles
are the Inter surface). 73 gates.

## Non-goals
Multiple sizes/weights; kerning pairs; non-ASCII coverage; using Inter for the
8×8 small-text call sites.
