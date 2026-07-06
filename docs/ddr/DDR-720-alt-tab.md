# DDR-720 — window cycling (Tab)

> DDR before code. Layer-7 polish: keyboard window cycling, the "alt-tab" of
> the deferred list — realized as plain **Tab** because the PS/2 keymap
> delivers ASCII only (`'\t'`, scancode 0x0F); modifier (Alt) tracking is a
> keymap-plumbing slice deferred with the rest of the scancode work.

## Decisions
- **D1 — Tab cycles focus to the bottom-most visible window.** The compositor
  (which already owns the raw key stream and routes non-hotkeys to the focused
  window, DDR-708) handles `'\t'`: pick the surface with the LOWEST z among
  visible (non-minimized, DDR-717) surfaces and `SYS_SURFACE_RAISE` it
  (top + focus; sovereign override applies). Repeated Tab therefore cycles
  through all windows in z-order — the classic MRU-inverse rotation.
- **D2 — announce + recompose.** Prints `PRADYOS_WM_CYCLE id=N` and
  recomposes; the existing frame loop then reports `PRADYOS_FOCUS`/`ZORDER`.
- **D3 — Tab is a compositor hotkey now** (like s/m/q/r): it is NOT forwarded
  to the focused window. Clients don't consume Tab today (PRISM reads the
  console, not surface keys).

## Gate
`smoke-alttab`: GPU boot + `input_inject` sends Tab twice → two
`PRADYOS_WM_CYCLE` lines with different ids (A and B swap as each raise puts
the other at the bottom). 65 gates.

## Non-goals
Real Alt-modifier chords (scancode/modifier plumbing); cycle overlays/HUD;
reverse cycling; per-workspace sets.
