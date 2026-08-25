# DDR-997 — resize from any edge, not just the bottom-right corner

**Status:** IMPLEMENTED, GATED, MUTATION-CHECKED (M1/M2/M3 all caught).
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


---

## 9. Implementation — what was measured

Kernel hash **`6f0da11f2ef4a123`**, `kernel.bin` 1,085,834 B against the
1,572,864 B gate. `make image` warning-clean at `-Werror`.

`smoke-resizeall` drives four drags in ONE boot and passes:

```
arm e OK — (140,140 64x64)   -> (140,140 157x64),  1 observation(s)
arm s OK — (140,140 157x64)  -> (140,140 157x117), 1 observation(s)
arm w OK — (140,140 157x117) -> (265,140 32x117),  1 observation(s)
arm n OK — (265,140 32x117)  -> (265,225 32x32),   1 observation(s)
```

The two load-bearing equalities hold exactly: `140+157 = 297 = 265+32` (W) and
`140+117 = 257 = 225+32` (N). Both shrink arms reached the 32 px floor, so the
clamp is genuinely exercised rather than merely present.

### 9.1 The FIX line had to report the OBSERVED origin, not the intended one

The first version emitted `x=`/`y=` from the compositor's own `newx`/`newy`.
Under M1 — drop the `SYS_SURFACE_MOVE` and change nothing else — `newx` is still
computed and would still have been printed, so **the gate would have passed a
window that never moved**. That is the identical decorative-arm mistake DDR-996's
first arm B made, caught here before it was believed rather than after.

The fix is a re-poll: `SYS_SURFACE_POLL` after the move, reporting the surface's
actual `x`/`y`. `w`/`h` stay the REQUESTED values, because the client honours the
resize asynchronously (that round-trip is `smoke-evresize`'s job). Mixing an
observed origin with a requested size is deliberate and is exactly the property
under test — the origin actually moved to must complement the width actually
asked for.

### 9.2 Mutation results — three mutants, three distinct kernel hashes

| Mutant | Kernel hash | Result |
|---|---|---|
| (none) | `6f0da11f2ef4a123` | `smoke-resizeall` PASS, `smoke-drag` PASS |
| **M1** — drop the move on a W/N drag | `34ef019aa3fdccd5` | W and N FAIL, **E and S still pass** |
| **M2** — clamp after deriving the origin | `c683670acf34792a` | W and N FAIL |
| **M3** — resize hit-test before the title bar | `018e1777db0547fb` | **`smoke-drag` FAILs** |

M1 and M2 fail with **different** signatures, so the gate discriminates them
rather than merely reporting "something is wrong":

- M1: `x+w=172, was x0+w0=297` — the origin never moved, so the right edge
  travelled LEFT by the amount the width shrank. The dedicated
  `origin did NOT move` check also fires.
- M2: `x+w=322, was x0+w0=297` — the origin moved to the UNCLAMPED pointer
  position and then the width was floored under it, so the right edge overshot
  RIGHT by exactly `32 - 7 = 25`. The `origin did NOT move` check correctly does
  NOT fire here: the origin did move, just to the wrong place.

E and S surviving M1 is the point §7 makes: an arm that only asserted "the width
changed" would have passed both mutants.

### 9.3 M3 is NOT vacuous, and the reason is worth recording

§2 says the title bar must be hit-tested first. On a single surface that
requirement is now structural rather than ordered: the title bar occupies
`y-TITLEBAR .. y` and the N band occupies `y .. y+RZBAND`, so the two regions are
**disjoint** and no ordering can change the outcome. Reading only that, M3 looks
untestable.

The ambiguity is **cross-surface**, and it is real in the shipped layout. ALPHA
sits at (100,100) 64x64, so its east band is `x >= 150` over `y 100..164`. BETA
sits at (140,140), so its published title-bar drag point `dg=` is (150,131) —
**inside ALPHA's east band**. Under M3 the press therefore grabs ALPHA's east
edge instead of moving BETA, and `smoke-drag` fails with `drag did not start on
the title bar` while the log carries the giveaway:

```
PRADYOS_RESIZE_FIX id=0 edge=8 x0=100 y0=100 w0=64 h0=64 x=100 y=100 w=300 h=64
```

id=0 is ALPHA, `edge=8` is `RZ_E`. Predicted from the published geometry before
the run, then confirmed by it.

### 9.4 One bug found in the gate itself, by the serial log

The first `smoke-resizeall` run had E, W and N green and **S failing on every
attempt** — a suspicious pattern, since S is the easiest arm (origin fixed, one
dimension). The compositor's own `PRADYOS_BTN_STATE` lines (DDR-941) settled it
without touching the resize code: the compositor observed **6 of the 10 injected
button edges**, and the missing pair was S's.

Cause was in the injector, not the kernel. It waited for "any new
`PRADYOS_WM_GEOM` line" between arms; in the capture that wait was satisfied by
an unrelated republish (GAMMA closing) **before the E arm's resize had even
committed**, so the S drag was injected while the compositor was still inside E's
client round-trip and recompose. `SYS_MOUSE_POLL` reads current state rather than
an event queue (DDR-941), so a press and a release that both fall inside one busy
window are not queued — they are simply never seen. The wait now requires a geom
line published *after* this arm's own drags.

This is §INV.8's lesson in a different costume: the failure was a claim about
timing, and reading the timing instrument first was cheaper than reading the code.

### 9.5 Geometry republish was stale, repo-wide

`PRADYOS_WM_GEOM` was emitted only when the surface COUNT or the focus changed,
so after any move or resize the last published line described a window that had
since moved. `smoke-evresize` never noticed because it does one drag and stops.
Four drags in a row do notice. The publish condition now also fires on a rect
change, tracked exactly (per-slot `x/y/w/h`), not hashed — a hash collision here
would silently republish nothing.

### 9.6 Regressions checked

`smoke-evresize`, `smoke-drag`, `smoke-wmclose`, `smoke-wmmax`, `smoke-wmmin`,
`smoke-mouse`, `smoke-agent-click`, `smoke-shell` (5/5), `smoke-blkmq`,
`smoke-rqstress-liveness`, `smoke-blk-integrity` — all PASS on
`6f0da11f2ef4a123`. `ci-shard-check` OK at **155 gates / 10 shards / 7 excluded**;
`ci-probe-rodata-check` OK.

### 9.7 What this does NOT claim

The four arms exercise one surface at 1024x768 with a client that honours resize
requests. Not covered: a client that ignores or partially honours a request, a
window dragged off-screen (`SYS_SURFACE_MOVE` clamping is untested here), and
simultaneous drags on two surfaces. The 512 ceiling stays untouched per §8.
