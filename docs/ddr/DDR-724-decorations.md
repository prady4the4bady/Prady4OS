# DDR-724 — window decorations (frame + drop shadow)

> DDR before code (committed together). Rounds out the window chrome: title
> bars + close/min/max boxes exist (DDR-715/717/719); this adds the frame and
> depth cue.

## Decisions
- **D1 — drop shadow:** 3 fading translucent strips (`blend_px`, α 0.22→0.10)
  along the right + bottom of the frame (title bar included) — a soft offset
  shadow, no full blur pass.
- **D2 — 1px frame:** accent-colored around the FOCUSED window, neutral gray
  (0x686060 BGR) around the rest — a glanceable focus cue. Off-screen edges
  clip naturally through `put_px`'s bounds check.
- **D3 — sentinel `PRADYOS_DECOR_OK`** on the first decorated window.

## Gate
`smoke-decor`: GPU boot; asserts `PRADYOS_DECOR_OK` + `PRADYOS_SURFACE_OK`.
69 gates.

## Non-goals
Rounded corners; blurred shadows; client-side decorations; resize handles
beyond DDR-718's pointer-resize.
