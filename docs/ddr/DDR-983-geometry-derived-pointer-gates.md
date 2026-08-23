# DDR-983 — §INV.5: publish the max box, and re-resolve geometry per click

Status: ACCEPTED — invariant violation removed, with a measured before/after.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Completes:** DDR-894 (`rz=`) and DDR-897 (`dg=`), which did this for two other
targets. **Answers:** the `smoke-wmmax` intermittent DDR-975 §7/§8 left open.

---

## 1. The violation

§INV.5 says pointer targets come from `PRADYOS_WM_GEOM` fields, never hardcoded
pixels. Three injection sites still hardcoded them:

```
smoke-wmmin   ABSX=5760  ABSY=5588
smoke-wmmax   ABSX=5311  ABSY=5588   and   ABSX=15424 ABSY=725
```

DDR-910 already recorded what this costs, for `smoke-wmclose`:

> *The gate used to hardcode pixels, which silently encoded the window creation
> order. Fair-share scheduling is free to change that order, and when item 16
> did, every click landed on empty space while the input path worked perfectly
> (23× `PRADYOS_MOUSE_OK`, zero `PRADYOS_WM_CLOSE`).*

That is the `smoke-wmmax` signature from DDR-975: intermittent, two different
failing assertions, 8/8 locally, input path healthy.

## 2. Why it had not already been fixed — the max box was never published

`GEOM_TITLE`/`GEOM_FIELD` has existed since DDR-910, and `smoke-wmclose` uses
it. `smoke-wmmax` could not: `PRADYOS_WM_GEOM` carried `close`, `min`, `rz` and
`dg`, and **no field for the max box**. The compositor computed `gmaxbox` — the
max box's left edge — purely to derive the drag strip's right boundary, and
never emitted it. With no field to name, the gate had to hardcode.

So this is not a gate that ignored an available mechanism; it is a mechanism
with a missing field, and the gate was the symptom.

**Fix:** publish `mx=` from `gmaxbox + CLOSEBOX/2`, with the `gby` that `close`
and `min` already share. `gmaxbox` is *literally* the `bx` expression from
`max_box_hit()` (`x + w - 3*CLOSEBOX - 8`), so the emitted target cannot drift
from what the hit-test accepts — the property DDR-894 established for `rz=`.

Appended, never inserted. Both consumers isolate fields by name —
`drag_inject.sh` uses `${geom##*dg=}` (DDR-935) and `mouse_inject.sh` scans
tokens with `startswith(field + "=")` — and `"mx="` cannot prefix-collide with
`"min="`.

## 3. The part a single change would have got wrong

Converting the three sites and running the gate **failed**:

```
[wmmax] FAIL — restore click did not un-maximize
```

with *both* injectors reporting the identical `mx=5317,5596`.

`resolve_geometry()` was called **once, before the first click**, and the retry
loop then re-clicked that same point forever. That is correct only for a target
that does not move. `smoke-wmmax`'s second injection targets the max box *after*
the window is maximized and relocated to (8,26) at 512×512 — a different place
entirely. Its readiness sentinel is the **client's** `PRADYOS_EV_RESIZE_OK`,
which fires when the client acks the resize, before the compositor has
necessarily re-composited and re-emitted `PRADYOS_WM_GEOM` for the moved window.
Resolving at that instant returns the pre-move coordinates.

**Fix:** re-resolve before every click, not just the first.
`resolve_geometry()` already takes the newest matching line ("newest wins:
layout can change") — the *caller* was the part that assumed a static target. A
failed re-resolve keeps the previous coordinates rather than falling back to a
default pixel, preserving the no-guessed-coordinate property.

**This is the interesting half of the DDR.** Deriving a target from published
geometry is not sufficient on its own: it also has to be derived *at the time of
the click*, or it is just a slower way of hardcoding.

## 4. Measured

| | before the re-resolve fix | after |
|---|---|---|
| `smoke-wmmax` | **FAIL** on run 1 — restore click did not un-maximize | **5/5 PASS** |

And the resolved targets are now correctly **distinct** per injection, which is
the direct evidence the re-resolve works:

```
geometry for BETA: mx=15438,726     <- post-maximize (old hardcode: 15424,725)
geometry for BETA: mx=5317,5596     <- pre-maximize  (old hardcode:  5311,5588)
```

Both land within ~6-14 tablet units of the values that were hardcoded, which is
the corroboration that the derivation is right — and the small offset is exactly
the margin that disappears the moment layout shifts.

N=5 per §6.3's fast tier: these are UI gates that touch neither the scheduler
nor the capability system. `smoke-wmclose` and `smoke-evresize` are unchanged
and already geometry-derived.

## 5. Not converted, and why

`smoke-agents` (`ABSX=29250 ABSY=5632`) clicks an **agent card**, not a window.
Cards are not surfaces and emit no `PRADYOS_WM_GEOM` line, so there is no field
to derive from. Converting it would need the agent panel to publish its own
geometry — real work, not a rename, and out of scope here. Named per directive
§2 rather than left as a silent exception: **`smoke-agents` remains the one
pointer gate still targeting a hardcoded coordinate.**
