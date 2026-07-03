# DDR-719 — Window maximize + geometry restore (Layer 7)

> DDR before code. The DDR-718 event channel makes maximize possible with the
> same authority model: the compositor requests the size; the owner redraws.

## Decisions

### D1 — Maximize toggle via the event channel
A third title-bar box (green, at `x+w-44`, left of min/close) toggles maximize:
- **Maximize:** the compositor saves the window's `{x,y,w,h}` (per-id arrays +
  `g_max_mask`), sends `SURF_EV_RESIZE_REQ(512,512)` — `SURFACE_DIM_MAX` is the
  per-surface buffer cap, so "maximized" = the largest legal surface — and
  repositions the window to `(8,26)` via `SYS_SURFACE_MOVE` (already
  sovereign-authorized, DDR-710). Prints `PRADYOS_WM_MAX id=<id>`.
- **Restore:** a second click on the (relocated) box sends
  `SURF_EV_RESIZE_REQ(saved w,h)` + `SYS_SURFACE_MOVE(saved x,y)`, clears the
  mask, prints `PRADYOS_WM_UNMAX id=<id>`.
The owner honors both through its existing DDR-718 resize handler. True
full-screen maximize waits on larger surface buffers (a later SURFACE_DIM bump).

**Commit keep-position sentinel (kernel):** the first gate run exposed that a
client re-commit stomps the compositor's `MOVE` (the client passed its stale
position). `SYS_SURFACE_COMMIT` now treats `x == SURF_POS_KEEP (INT32_MAX)` as
"keep the current position" — placement stays compositor-owned across
event-channel resizes; the DDR-718 client handler re-commits with KEEP.

### D2 — Hit-test order and the crowded 64-px title bar
Box order right-to-left: close (`w-16`), min (`w-30`), **max (`w-44`)**; checks
run max → min → close → drag. A 64-px window keeps a 20-px drag region
(x..x+20). **`smoke-drag`'s default click at pixel 160 now lands on B's max
box**, so `drag_inject.sh`'s default start moves to pixel (150,130) — abs
(4800, 5546) — still on B's title-bar drag region; `smoke-drag` re-verified.

## Gate
`smoke-wmmax` (CI, GPU + tablet, two sequential QMP injections): click B's max
box (readiness `PRADYOS_AMBIANCE_OK`) → `PRADYOS_WM_MAX id=1` +
`PRADYOS_EV_RESIZE_OK w=512 h=512`; then a second injection at the relocated
box (readiness `PRADYOS_WM_MAX`) → `PRADYOS_WM_UNMAX id=1`. 52 CI gates total.

## Non-goals (later)
Full-screen maximize (needs SURFACE_DIM_MAX > 512); double-click-to-maximize;
tiling/snapping; animations.
