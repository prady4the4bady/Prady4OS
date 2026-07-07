# DDR-722 — real glass blur + saturation

> DDR before code. The brief's §9 frosted glass, deferred since DDR-712 as a
> flat tint. Glass cards now BLUR the composed scene beneath them (separable
> box blur) and boost saturation — the actual "frosted" look.

## Decisions
- **D1 — separable in-place box blur.** `glass_card` first blurs its rect on
  the framebuffer: a horizontal 9-tap (radius 4) box pass into a row buffer,
  written back, then the same vertically. O(w·h·taps), card-sized regions only
  (~2 cards/frame) — fine on the software path.
- **D2 — saturation boost.** After the blur, each pixel's chroma is scaled
  ×1.3 around its luma (gray = (r+g+b)/3, c' = gray + (c−gray)·1.3, clamped) —
  the brief's "blur + saturation" pair.
- **D3 — the tint + accent border stay** (drawn over the blurred backdrop, as
  designed). `PRADYOS_GLASS_OK` (smoke-visual) is unchanged; a new one-shot
  `PRADYOS_GLASS_BLUR_OK` prints on the first blurred card.

## Gate
`smoke-glassblur`: GPU boot; asserts `PRADYOS_GLASS_BLUR_OK` +
`PRADYOS_GLASS_OK`. 67 gates.

## Non-goals
Gaussian kernels; per-frame full-screen blur; GPU-accelerated paths; blur
under client windows (cards only, per the brief).
