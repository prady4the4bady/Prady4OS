# DDR-717 — Window minimize + restore (Layer 7)

> DDR before code. DDR-715 finished the close button; minimize is next. Restore
> UI real estate (a dock) doesn't exist yet, so restore is a compositor shortcut.

## Decisions

### D1 — Minimize is compositor-local state (no kernel change)
Minimizing only means "don't composite this window" — the surface, its buffer,
and its committed state are untouched. The compositor keeps a `g_min_mask`
bitmask by surface id: minimized windows are skipped when drawing **and** when
title-bar hit-testing (their pixels aren't on screen). `SYS_SURFACE_RESIZE`
stays owner-only; nothing in the kernel moves. (Pointer **resize handles** and
**maximize** both require the client to redraw at a compositor-chosen size —
i.e. a surface **event channel** to the owner — deferred to a future DDR.)

### D2 — Minimize box in the title bar
A second 12×12 box (amber) sits 2 px left of the close box. The button-down
hit-test order becomes: close box → **min box** → title-bar drag → plain click.
A min hit sets the mask bit, prints `PRADYOS_WM_MIN id=<id>`, and recomposites
(the window disappears; its surface stays committed).

### D3 — Restore-all on the `r` key
The compositor's keyboard loop gains `r`: clear `g_min_mask`, print
`PRADYOS_WM_RESTORE`, recomposite. (`r` was previously routed to the focused
window like any other key; windows lose one letter until a dock exists —
acceptable, documented here.) Per-window restore/dock stays deferred.

## Gate
`smoke-wmmin` (CI, GPU + tablet + HMP): the mouse injector clicks **B's min
box** (readiness `PRADYOS_AMBIANCE_OK`), then the key injector (waiting on
`PRADYOS_WM_MIN`) sends `r`. Greps `PRADYOS_WM_MIN id=1` + `PRADYOS_WM_RESTORE`.
50 CI gates total. `smoke-drag`'s title-bar click at x=160 stays left of B's
min box (x≥174); all other gates unaffected.

## Non-goals (later)
Maximize; per-window restore (needs a dock/taskbar); minimize animation; the
surface event channel (unblocks resize handles + maximize).
