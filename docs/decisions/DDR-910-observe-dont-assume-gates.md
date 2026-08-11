= DDR-910 — pointer gates must observe geometry, not assume pixels

**Status:** Step 1 (polling injector) SHIPPED. Step A (geometry emission)
designed, not yet implemented.
**Date:** 2026-08-11

## The defect class

Two gate failures this session had the same root shape, one axis apart:

| | assumed | should observe |
|---|---|---|
| fixed sleep | that 0.5s was enough time | the outcome sentinel |
| hardcoded pixels | that GAMMA is at 16190,2602 | the compositor's reported geometry |

Both are "assume instead of observe". Item 16's fair-share pick changed the
order client windows are created in — a legitimate consequence of fair-share
scheduling — and the pixel assumption broke. **A gate depending on the old order
was depending on undocumented FIFO behaviour.** Item 16 is not at fault.

## Step 1 — DONE

`mouse_inject.sh` re-clicks until the pattern proving the click was acted on
appears, exits the moment it is observed, and reports a genuine timeout
otherwise. Result: `smoke-wmmin` PASS (observed after 4 clicks);
`smoke-wmclose` FAIL with `TIMEOUT after 50 click(s)` — which is what
established that wmclose is non-response, not slowness.

## Step A — the design, ready to apply

The compositor currently emits **no geometry at all**: only `PRADYOS_TITLE_OK`
and `PRADYOS_ZORDER 0 1`. There is nothing to derive coordinates from, so the
observable state must first be made observable.

Authoritative rectangle, from `user/compositor.c` (`draw_window`), which is the
same computation the hit-test uses — the emission MUST come from here, or the
gate merely assumes differently:

```c
#define CLOSEBOX 12                       /* line 581 */
int ty = s->y - TITLEBAR; if (ty < 0) ty = 0;
int tx = s->x < 0 ? 0 : s->x;
/* close box */ (tx + s->w - CLOSEBOX - 4,     ty + 3, 12, 12)
/* min box   */ (tx + s->w - 2*CLOSEBOX - 6,   ty + 3, 12, 12)
/* max box   */ (tx + s->w - 3*CLOSEBOX - 8,   ty + 3, 12, 12)
```

Centres: close `(tx + s->w - 10, ty + 9)`, min `(tx + s->w - 24, ty + 9)`.

**Emit already-scaled tablet coordinates, not screen pixels.** The compositor
knows the framebuffer dimensions; the gate does not. Scaling in the gate would
duplicate the mapping and create a second thing to get wrong.

```
PRADYOS_WMGEOM id=%u title=%s close=%d,%d min=%d,%d max=%d,%d
```
with each pair pre-scaled to the virtio-tablet 0..32767 range
(`v * 32767 / (dim - 1)`). Emit alongside the existing `PRADYOS_ZORDER` block so
it is refreshed whenever the live set or focus changes.

The gate then greps for the line with `title=GAMMA`, takes `close=`, and passes
it to the Step 1 injector as ABSX/ABSY. Immune to creation order, window count,
z-order, and any future scheduler, allocator or startup change.

**Verification bar:** `PRADYOS_WM_CLOSE` must appear in a *small* number of
clicks. A geometry fix that still needs many retries means a second defect and
must not be reported as fixed.

## Not to be done

Nudging `ABSX`/`ABSY` until they hit again. That leaves the gate equally brittle
and still does not prove the close path works — the same reasoning that ruled out
enlarging the sleeps.

## smoke-winops — explicitly NOT covered here

Client-driven, no injector, already polls with a 90s bound. A third, independent
question. It must be diagnosed from its own client source and bisected on its
own, and must not be assumed to share a cause with wmclose or item 16.
`TIMEOUT_S` is not to be raised.
