# DDR-709 — Sun-driven ambiances (OKLab) + animated toggle (Layer 7)

> DDR before code, per the brief. Implements the brief's signature UI: the four
> time-of-day ambiances (§1) with **OKLab** colour interpolation (§2) and the
> animated Sovereign/Manual toggle (§3). Scoped to the *colour/transition* core;
> the full visual detail (particle fields, glass blur, multi-stop gradients, the
> Inter font) stays deferred — this delivers the perceptually-correct ambiance
> engine those layer onto.

## Decisions

### D1 — Four ambiance palettes (compositor)
The compositor holds 4 ambiances (DAWN/DAY/DUSK/NIGHT), each a representative
background + accent colour from the brief §1:
- DAWN  `#1A0A2E` bg / `#C8A4E8` accent  (05:00–08:59)
- DAY   `#0D1B2A` bg / `#4FAEFF` accent  (09:00–16:59)
- DUSK  `#3D1500` bg / `#FFB347` accent  (17:00–20:59)
- NIGHT `#000008` bg / `#7B4FE0` accent  (21:00–04:59, Sovereign home ground)
The desktop background fill + accent bar use the **current interpolated** ambiance
colour; the Sovereign/Manual label/mode overlay is unchanged (DDR-704). The agent
panel + windows compose on top as before.

### D2 — OKLab interpolation (genuine, libm-free)
Colour transitions interpolate in **OKLab**, not sRGB (the brief's explicit
requirement — sRGB lerps pass through muddy mid-greys). `user/compositor.c` gains a
self-contained sRGB↔OKLab transform with a **Newton-iteration `cbrt`** (the musl
subset has no libm). A transition lerps `oklab(current) → oklab(target)` over N
frames and converts back to sRGB per frame. (The linear-light EOTF is approximated;
the perceptually-important LMS cube-root step is exact.)

### D3 — Time-of-day selection via a small RTC clock syscall
The vDSO clock is monotonic-since-boot, not wall-clock — so a new read-only syscall
exposes the RTC:
```
SYS_CLOCK() -> seconds-since-midnight (0..86399), from rtc_now()   [57]
```
The compositor maps the hour to an ambiance (the §1 boundaries) at startup and as
time advances. Real wall-clock can't be made deterministic in headless CI, so the
**gate** drives the ambiances explicitly (D5) rather than relying on the clock.

### D4 — Animated Sovereign/Manual toggle
On a mode flip (`s`/`m`), the accent colour animates from the old to the new tint
over ~12 frames in OKLab (≈600 ms feel), re-rendering + presenting each step, then
prints `PRADYOS_TOGGLE_ANIM_OK`. Event-driven (only during the animation).

### D5 — Gate
`smoke-ambiance` (CI, `QEMU_GPU=1`): on startup the compositor runs a one-time
**demo cycle** through the four ambiances with OKLab transitions (independent of
the wall clock, for determinism), printing `PRADYOS_AMBIANCE DAWN|DAY|DUSK|NIGHT`
per step and `PRADYOS_AMBIANCE_OK` when done; it then settles on the
time-of-day-selected ambiance. The gate greps all four names + `PRADYOS_AMBIANCE_OK`
(+ `PRADYOS_TOGGLE_ANIM_OK` is covered by the existing keyboard-driven toggle).

## Non-goals (later)
Particle fields (motes/embers/stars), real glass blur + saturation, multi-stop
background gradients + sun-bloom radials, the Inter typeface, the 15-min-before
boundary pre-transition + 900 s auto cadence, the toggle's spring/ripple motion,
and `prefers-reduced-motion` — all deferred. This is the OKLab ambiance colour
engine + time selection + the toggle colour animation.
