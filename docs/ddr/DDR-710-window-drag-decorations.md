# DDR-710 — Window decorations + drag-to-move (Layer 7)

> DDR before code, per the brief. Windows (DDR-706/708) are static rectangles. This
> slice adds a **title bar** decoration per window and **drag-to-move** with the
> pointer — grab a title bar, drag, drop. Completes basic direct-manipulation
> windowing.

## Decisions

### D1 — Compositor-settable window position
Moving a window means changing its `(x, y)`. `SYS_SURFACE_COMMIT` is owner-only, so
a new syscall lets the **compositor** (the trusted UI process) reposition any
window:
```
SYS_SURFACE_MOVE(id, x, y) -> 0 | -errno    [58]
```
Allowed for the surface owner OR a `CAP_SOVEREIGN` caller (the compositor). Updates
`g_surf[id].{x,y}`; the next `SURFACE_POLL` reflects it.

### D2 — Window decorations (title bar) drawn by the compositor
The compositor draws a **title bar** strip (`TITLEBAR = 18 px`) directly above each
client surface — at `(x, y-18, w, 18)` in the ambiance accent colour, with the
surface content below. The title bar is the **drag handle**; the surface content is
the client's. (Close/min buttons + a real title string are deferred.)

### D3 — Drag-to-move (absolute pointer)
The compositor tracks the pointer (`SYS_MOUSE_POLL`, absolute). On **button-down**,
it hit-tests the **topmost** window whose title-bar rect contains the pointer; if
found it raises+focuses that window (`SYS_SURFACE_RAISE`), starts a drag recording
`offset = pointer - window.origin`, and prints `PRADYOS_DRAG_START id=<id>`. While
the button stays down and the pointer moves, it sets the window position to
`pointer - offset` via `SYS_SURFACE_MOVE` and re-composites (event-driven). On
**button-up** it ends the drag and prints `PRADYOS_DRAG id=<id> x=<x> y=<y>`. A
button-down **not** on a title bar keeps the existing behaviour (cursor +
`PRADYOS_MOUSE_OK`).

## Gate

`smoke-drag` (CI, `QEMU_GPU=1`): the client (`surfacetest`) creates windows A and B
(B raised). The harness drives a drag via **QMP `input-send-event`**: move the
pointer onto **B's title bar**, button-down, move to a new location (button held),
button-up. The compositor moves B and prints `PRADYOS_DRAG id=<B> x=… y=…`. The
gate greps `PRADYOS_DRAG_START` + `PRADYOS_DRAG id=`. (Existing `smoke-mouse` still
covers a plain click via the no-title-bar path.)

## Non-goals (later)
Close/minimise/maximise buttons; a real title string per window; resize handles;
snapping / tiling; drag inertia; double-click-to-maximise; window shadows; the
glass title-bar styling. Deferred. This is decorations + drag-to-move.
