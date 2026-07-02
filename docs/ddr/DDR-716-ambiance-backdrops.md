# DDR-716 — Per-ambiance backdrops: DAY mesh, DUSK sun-bloom, NIGHT nebulas (Layer 7)

> DDR before code. DDR-712 shipped the particle layer; the brief's §1 backdrop
> features stayed deferred: DAY's 3-node gradient mesh, DUSK's sun-bloom radial,
> and NIGHT's nebula radials. This slice adds them as software radial gradients in
> the compositor, completing the four ambiances' signature backgrounds.

## Decisions

### D1 — One primitive: a bounded radial glow, sqrt-free
`radial_glow(cx, cy, r, colour, base_alpha)` alpha-blends a disc onto the
framebuffer with a quadratic falloff `a = base_a * (1 - d²/r²)` — no sqrt, one
multiply-compare per pixel, clipped to the screen. It reuses DDR-712's
`blend_px`. Cost is bounded by the disc area, which motivates D3.

### D2 — Per-ambiance composition (brief §1)
Drawn in `render()` after the background fill and **before** particles/windows:
- **DAWN** — no radials (the mote field carries it; horizon line deferred).
- **DAY** — the "3-node mesh" approximated as three soft radial nodes in the
  day palette (light blue / white / deep blue) at fixed thirds of the screen;
  the brief's 20 s mesh *animation* stays deferred.
- **DUSK** — one sun-bloom at (85%, 90%) of the screen, warm orange
  (`255,120,30` @ 0.25), r = 35% of width.
- **NIGHT** — two nebulas: `#120024` at (30%, 40%) r600-scaled and `#001220`
  at (70%, 60%) r500-scaled (radii scaled to width/1024); the 120 s drift stays
  deferred.

### D3 — Only on settled frames (perf guard)
Radial fills are the most per-pixel work the software compositor does (~1–2 M
blended px for NIGHT). They are therefore drawn only when `render()` is called
with the ambiance **settled** — not on the 6–8 intermediate frames of an OKLab
transition. `set_ambiance` passes a `final` flag; mid-lerp frames keep today's
flat background, so transition latency (and the 90 s gates) are unaffected. On
the first settled render of each ambiance the compositor prints
`PRADYOS_BACKDROP <NAME>` once, and `PRADYOS_BACKDROP_OK` after all four have
been seen (the startup demo cycle guarantees all four).

## Gate
`smoke-backdrop` (CI, `QEMU_GPU=1`, client-driven): boot and grep
`PRADYOS_BACKDROP DAY`, `PRADYOS_BACKDROP DUSK`, `PRADYOS_BACKDROP NIGHT` +
`PRADYOS_BACKDROP_OK`. 49 CI gates total. All existing gates unaffected —
backdrops draw beneath particles, label, panel, and windows.

## Non-goals (later)
Mesh/nebula animation (20 s / 120 s drifts); the DAWN/DUSK horizon bands; real
Gaussian blur + saturation; multi-stop linear gradients; per-star glow bloom
beyond DDR-712's. Deferred.
