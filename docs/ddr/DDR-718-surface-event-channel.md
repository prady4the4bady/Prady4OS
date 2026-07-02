# DDR-718 — Surface event channel + pointer resize (Layer 7)

> DDR before code. DDR-715/717 deferred maximize and resize handles because the
> compositor cannot resize a window: `SYS_SURFACE_RESIZE` is owner-only **by
> design** (the owner must redraw into the new buffer). The missing piece is a
> compositor→client event channel. This slice adds it, plus its first consumer:
> **drag-resize from the window's bottom-right corner**.

## Decisions

### D1 — Per-surface event ring, two syscalls
Each surface gains a small typed-event ring (8 entries of
`{u16 type, u16 arg0, u16 arg1}`), alongside the DDR-708 key ring:
- `SYS_SURFACE_SENDEV (62)` — push an event (compositor `CAP_SOVEREIGN` or
  owner), drop-on-full like the key ring.
- `SYS_SURFACE_GETEV (63)` — owner drains one event into a copyout'd struct;
  returns 0 or `-EAGAIN` when empty.
Event type 1 = `SURF_EV_RESIZE_REQ (w, h)`. Authority is unchanged: the
compositor only *requests*; the owner performs `SYS_SURFACE_RESIZE`, re-maps,
redraws, and re-commits. (This is the Wayland `configure` shape in miniature.)

### D2 — Corner drag-resize in the compositor
A button-down inside a window's **bottom-right 14×14 corner** (topmost first,
checked after the title-bar cases — the corner is in the content area) starts a
resize drag: pointer moves track a rubber outline (just the cursor for now);
button-up computes `neww/newh` (clamped 32..512) and sends
`SURF_EV_RESIZE_REQ`, printing `PRADYOS_RESIZE_REQ id=<id> w=<w> h=<h>`. The
recomposite happens when the client re-commits (the poll data changes size —
the compositor repaints on the next loop via the focus/set detector plus an
explicit repaint on button-up).

### D3 — Client handling (surfacetest)
The key-drain loop also drains events: on `SURF_EV_RESIZE_REQ` window B calls
`SYS_SURFACE_RESIZE`, re-maps, redraws its fill at the new size, re-commits at
its current position, and prints `PRADYOS_EV_RESIZE_OK w=<w> h=<h>`.

### D4 — Harness
`drag_inject.sh` gains optional `SX/SY/EX/EY` abs-coordinate env overrides
(defaults = the DDR-710 title-bar drag, so `smoke-drag` is untouched).

## Gate
`smoke-evresize` (CI, GPU + tablet, QMP): drag from B's corner
(pixel ≈(198,198)) to ≈(300,260); grep `PRADYOS_RESIZE_REQ id=1` +
`PRADYOS_EV_RESIZE_OK`. 51 CI gates total. Existing gates unaffected: the
corner region wasn't previously interactive (a click there was a plain click,
which nothing asserts on), and `smoke-drag`/`smoke-mouse` coordinates avoid it.

## Non-goals (later)
Maximize (needs saved-geometry restore; trivially built on this channel next);
live rubber-band preview; min-size hints from the client; close-request events;
event coalescing.
