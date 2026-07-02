# DDR-715 — Per-window title strings + close button (Layer 7)

> DDR before code. DDR-710/711 deferred "a real title string per window" and
> "close buttons"; this slice ships both, completing direct-manipulation
> windowing: every window shows its name in the title bar and can be closed with
> the pointer.

## Decisions

### D1 — `SYS_SURFACE_SET_TITLE(id, str) -> 0 | -errno`  [61]
Owner-only (a window names itself). The kernel `copyinstr`s up to **15 chars +
NUL** into a new `title[16]` field of the surface table, and `SURFACE_POLL`'s
`struct surface_info` gains the same `title[16]` at the end — kernel, compositor,
and surfacetest share the struct and are updated in the same commit (all
in-tree; no external ABI).

### D2 — Compositor renders the title in the title bar
`draw_window` draws the title string (scale 1, background-contrast colour) inside
the DDR-710 title bar, left-aligned after a 6 px inset. The embedded 8×8 font
gains the missing glyphs **B, C, T, W** (titles in-tree use the covered
alphabet). Empty title → bar stays as today.

### D3 — Close button + click-to-close
The compositor draws a **12×12 close box** at the right end of each title bar
(accent-contrast red). The pointer button-down hit-test checks the close box
**before** the drag-start check: a hit calls `SYS_SURFACE_CLOSE(id)` (the
compositor is `CAP_SOVEREIGN`; DDR-711 D1 already authorizes it), prints
`PRADYOS_WM_CLOSE id=<id>`, and recomposites — the DDR-711 shrink detector then
also reports `PRADYOS_SURFACE_GONE`. A click on the title bar left of the box
still starts a drag (DDR-710); everything else is unchanged.

### D4 — Test driver
`surfacetest` titles its windows (`SYS_SURFACE_SET_TITLE`: A="ALPHA", B="BETA",
C="GAMMA") and prints `PRADYOS_TITLE_OK` once set. The close-button gate closes
**window C** (the DDR-711 throwaway window, before its timed self-close) so A/B
and every existing gate are untouched; C's self-close path stays as the fallback
covered by `smoke-winops`.

## Gate
`smoke-wmclose` (CI, GPU + tablet, QMP): wait for the desktop, click **C's close
box**, grep `PRADYOS_TITLE_OK` + `PRADYOS_WM_CLOSE id=2` + `PRADYOS_SURFACE_GONE`.
48 CI gates total. `smoke-winops` keeps passing (C self-closes if nothing clicks
it); `smoke-drag`/`smoke-focus` unaffected (B's title-bar drag region shrinks by
the 12 px box, and the drag gate clicks the bar's left half).

## Non-goals (later)
Minimise/maximise buttons; window shadows; hover states on the button; focus
follows close (next-in-z gets focus); title truncation with ellipsis; UTF-8.
