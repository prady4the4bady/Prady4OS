= DDR-894 — `smoke-evresize` must observe the resize corner, not hardcode it

**Status:** ACCEPTED.
**Date:** 2026-08-15
**Lineage:** DDR-718 (corner resize) + DDR-910 (observe, don't assume) →
**DDR-894 (this)**. Completes DDR-910's coverage for the one gate it missed.

## The flake

`smoke-evresize` failed in CI run 31898538294 (shard 3):

```
[evresize] FAIL — corner drag did not request a resize
```

i.e. `PRADYOS_RESIZE_REQ id=1` never appeared.

## Root cause — a 14x14 pixel target hit by hardcoded coordinates

The resize hit-test (`user/compositor.c:1106-1112`, DDR-718) accepts only the
bottom-right **14x14 pixel** corner of a surface:

```c
int cx0 = sf[i].x + (int)sf[i].w - 14, cy0 = sf[i].y + (int)sf[i].h - 14;
if (ms.x >= cx0 && ms.x < sf[i].x + (int)sf[i].w &&
    ms.y >= cy0 && ms.y < sf[i].y + (int)sf[i].h) {
```

The gate injects **fixed** absolute tablet coordinates:

```
SX=6303 SY=8404 EX=9599 EY=11090 bash tools/qemu_runner/drag_inject.sh …
```

So the gate passes only while the window happens to sit where those constants
assume. Any run-to-run variation in surface position or size — creation order,
scheduling, focus, an earlier gate's leftover state — moves a 14-pixel target
and the press lands on empty desktop, starting neither a drag nor a resize.

**This is exactly the defect DDR-910 was written for**: *"hardcoded pixels …
should observe the compositor's reported geometry"*, and *"a gate depending on
the old order was depending on undocumented FIFO behaviour."*

Two things ruled out by reading, so they are not chased:

- **The injector is correct.** `drag_inject.sh` does press → move → release
  (`btn(True)` … `btn(False)`), and `PRADYOS_RESIZE_REQ` is printed on release
  (`compositor.c:1139-1145`, `else if (up && resizing)`). The sequence matches
  what the compositor expects.
- **No mode gate.** The resize path is not behind a sovereign/manual check.

## Why this gate was missed by DDR-910

DDR-910's Step A **did** ship — `PRADYOS_WM_GEOM` is emitted per surface
(`compositor.c:952-961`) carrying the close and min box centres, pre-scaled to
tablet coordinates, computed from the same expressions `draw_window` uses. But
it publishes only those two boxes. The **resize corner was never added**, so
`smoke-evresize` had nothing to observe and kept its constants.

## Decision

Extend the existing `PRADYOS_WM_GEOM` line with the resize corner centre,
derived from the same expression as the hit-test so the emitted target cannot
drift from what the hit-test accepts:

```
PRADYOS_WM_GEOM id=%u title=%s close=%d,%d min=%d,%d rz=%d,%d
```

with `rz` = centre of the 14x14 corner = `(x + w - 7, y + h - 7)`, scaled by the
same `tab_x`/`tab_y` helpers as the other fields.

`smoke-evresize` then reads `rz=` for its surface and injects there, instead of
`SX`/`SY` constants. The drag *end* point stays a relative offset from the
observed start, so the drag distance is preserved without reintroducing an
absolute assumption.

### Sentinel safety

`PRADYOS_WM_GEOM` already exists and is already consumed; appending a field to
the end of the line does not change the existing `close=`/`min=` parses.
`sentinel_collision.sh` is run regardless, per the standing rule.

## What this does NOT do

It does not change the resize behaviour, the hit-test size, or the 14-pixel
target. It makes the gate aim at where the compositor actually drew the corner.
If `smoke-evresize` still fails after this, the cause is in the resize path
itself and not in the aiming — which is precisely the distinction the current
gate cannot make.
