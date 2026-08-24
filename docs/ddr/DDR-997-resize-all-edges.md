# DDR-997 — resize from any edge, not just the bottom-right corner

**Status:** DESIGN. Not implemented.
**Extends:** DDR-718 (bottom-right 14x14 resize corner), DDR-894 (`rz=` geometry).
**Gate:** `smoke-resizeall` (new), alongside the existing `smoke-evresize`.

---

## 1. What exists, and the one thing that makes this non-trivial

`user/compositor.c:1260` hit-tests a single 14x14 square at the bottom-right of
each window. On release (`:1293`):

```c
int neww = ms.x - rs_bx, newh = ms.y - rs_by;   /* rs_bx/by = the surface ORIGIN */
```

That works because the bottom-right drag **leaves the origin fixed**: only width
and height change, and one `SYS_SURFACE_SENDEV(type 1)` carries both.

A north or west drag does not have that property. Pulling the left edge leftwards
must *both* widen the window *and* move its origin left. The compositor has two
separate calls for those — `SYS_SURFACE_MOVE` and the type-1 resize event — so
the operation is **not atomic**, and the order is a real decision rather than a
detail (§3).

This is the whole reason the item was left as "bottom-right only" and is worth a
DDR rather than a patch.

## 2. Handles

Eight regions per window, 14 px thick (the DDR-718 corner size, kept so the hit
target does not change size between old and new handles):

```
NW  N  NE      corners: 14x14 squares
 W  .  E       edges:   14 px deep strips between the corners
SW  S  SE
```

SE keeps its existing behaviour bit-for-bit — it is the one path with a green
gate today (`smoke-evresize`), and it must not regress.

The title bar already owns move-drag (DDR-705), so the N strip is taken from the
window's top edge ABOVE the title bar is NOT available. **N and NW/NE are
therefore hit-tested on the frame edge only, and the title-bar test runs FIRST**
— an ambiguous pixel belongs to move, not resize, because move is the older and
more frequently used gesture.

## 3. The origin-shift decision: MOVE first, then resize

For a W or N drag both a move and a resize are required. Two orders are possible
and they are not equivalent:

- **resize then move** — the window first grows/shrinks about its *old* origin,
  then jumps. For a leftwards W drag it visibly grows to the RIGHT and then
  snaps left: a one-frame artifact in the wrong direction.
- **move then resize** (CHOSEN) — the origin lands first, then the size follows.
  The intermediate frame is the window at its new position with its old size,
  which is the same shape a plain move already produces, so it reads as a move
  that then settles.

Neither is atomic and this DDR does not pretend otherwise. The choice is which
intermediate frame is less wrong, and "already looks like an existing gesture"
wins over "moves opposite to the pointer".

The client redraws asynchronously either way (DDR-718's `recompose_scene()`
comment already says so), so the intermediate is bounded by the client's
response, not by the compositor.

## 4. Clamps, and a trap

`neww`/`newh` clamp to [32, 512] exactly as DDR-718 does — 512 is
`SURFACE_DIM_MAX` (`sys_surface.c:17`), not a compositor preference.

**The trap:** on a W or N drag the clamp must be applied BEFORE deriving the new
origin, not after. Clamping the size afterwards leaves the origin where the
unclamped drag put it, so a window dragged past the 32 px floor keeps sliding
while its width stays pinned — the edge separates from the pointer. Derive the
clamped size first, then place the origin from the FIXED edge:

```
W drag:  neww = clamp(x0 + w0 - mx);  newx = (x0 + w0) - neww;
N drag:  newh = clamp(y0 + h0 - my);  newy = (y0 + h0) - newh;
```

The fixed edge (right for W, bottom for N) is the invariant, and both the size
and the origin are derived from it. That makes the clamp self-consistent.

## 5. Geometry must be published, not hardcoded (§INV.5 / §NON-NEGOTIABLE 9)

`PRADYOS_WM_GEOM` already carries `rz=X,Y` for the SE corner (DDR-894). Gates
must never hardcode pixel coordinates, so the new handles are published the same
way — one field per handle, each the CENTRE of its hit region:

```
rzn=X,Y rzs=X,Y rzw=X,Y rze=X,Y rznw=X,Y rzne=X,Y rzsw=X,Y
```

`rz=` is left alone, meaning SE, so every existing parser keeps working. §INV.5's
warning applies: a parser must isolate each field before splitting on `,`.

## 6. Gate — `smoke-resizeall`

For each of the four edges, drive a drag from the published handle centre and
assert the committed geometry:

- **E / S** — origin unchanged, one dimension changed. The cheap arms.
- **W** — `PRADYOS_RESIZE_REQ` width changed AND a `PRADYOS_DRAG`/move to the new
  origin, with `x_new + w_new == x_old + w_old` (the right edge held still).
  That equality is the assertion; a width-only check passes on a broken origin.
- **N** — the same with `y_new + h_new == y_old + h_old`.

The invariant-based arms (W, N) are the load-bearing ones: they encode §4's fixed
edge, so a mutant that resizes without moving, or moves without resizing, fails
them. A test that only asserted "width changed" would pass both mutants.

## 7. Mutation checks (required)

- **M1** — drop the move on a W drag. The W arm must fail on the fixed-edge
  equality while E/S still pass.
- **M2** — apply the clamp after deriving the origin (§4's trap). Drag past the
  32 px floor; the fixed-edge equality must break.
- **M3** — hit-test resize before the title bar. Move-drag on the title bar must
  break, catching the §2 ordering.

## 8. NOT in scope

- No aspect-ratio locking, no snapping, no keyboard resize.
- No minimum-size negotiation with the client: 32 px is imposed, as DDR-718 does.
- The 512 ceiling stays. Lifting it is `SURFACE_DIM_MAX` and a PMM budget change
  across many gates — sized separately, deliberately not bundled here.
