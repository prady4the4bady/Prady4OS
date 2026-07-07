# DDR-727 — spring toggle + click ripple motion

> DDR before code (committed together). The last animation item from the
> brief: the mode toggle gets a SPRING (overshoot) pulse instead of the linear
> ramp, and pointer clicks get an expanding RIPPLE.

## Decisions
- **D1 — spring easing:** `animate_toggle`'s linear white pulse becomes a
  damped-spring amplitude table (overshoot past the peak, settle back):
  precomputed 10-frame sequence `{0.3,0.6,0.85,1.0,1.08,1.02,0.97,1.0,0.5,0}`
  applied to the OKLab lerp toward white. Prints `PRADYOS_SPRING_OK` (keeps
  the existing `PRADYOS_TOGGLE_ANIM_OK` for the old gate).
- **D2 — click ripple:** on button-down the compositor draws 4 frames of an
  expanding 1px circle (radius 6→24, alpha 0.5→0.1, `blend_px`) at the click
  point, then recomposes. Prints `PRADYOS_RIPPLE_OK` once.
- **D3 — gates:** new `smoke-motion` (sendkey `s` → mode toggle →
  `PRADYOS_SPRING_OK`); `smoke-mouse` additionally asserts
  `PRADYOS_RIPPLE_OK` (its harness already clicks). 72 gates.

## Non-goals
Window open/close scale animations; easing curves beyond the fixed table;
the Inter typeface (the one remaining deferred visual).
