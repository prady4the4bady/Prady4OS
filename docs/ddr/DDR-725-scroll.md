# DDR-725 — scroll-wheel input plumbing

> DDR before code. The deferred "relative-mouse + scroll" item, scoped to the
> half that matters on the tablet-driven desktop: WHEEL scrolling end-to-end
> (true relative pointer motion is moot while QEMU exposes an absolute tablet).

## Decisions
- **D1 — driver:** `virtio_input.c`'s `fold_event` gains `EV_REL` (type 2) /
  `REL_WHEEL` (code 8): accumulate `value` (±1 per detent) into a per-device
  `g_wheel` counter.
- **D2 — syscall:** `SYS_MOUSE_POLL`'s `struct mouse_state` gains a trailing
  `int wheel` field — ABI-compatible append (existing callers pass a struct
  the kernel writes; the kernel must only write `wheel` if the user buffer is
  large enough → the syscall gains a size-aware copy or keeps the struct fixed
  and ALL in-tree callers recompile — we own every caller, so: append field,
  recompile world; note in the DDR). The kernel reports the accumulated wheel
  delta since the last poll (read-and-clear).
- **D3 — compositor routing:** on `wheel != 0`, send a surface EVENT
  (`SYS_SURFACE_SENDEV`, the DDR-718 channel) of `type 2` with
  `arg0 = (unsigned)delta` to the FOCUSED window, and print
  `PRADYOS_SCROLL d=<delta>` once per burst. surfacetest handles `ev.type==2`
  → prints `PRADYOS_EV_SCROLL_OK d=<n>`.
- **D4 — gate `smoke-scroll`:** the mouse_inject harness sends wheel via HMP
  `mouse_move 0 0 <dz>`; asserts `PRADYOS_SCROLL` (compositor saw the wheel)
  and `PRADYOS_EV_SCROLL_OK` (the focused client received it). 70 gates.

## Non-goals
Relative pointer motion (absolute tablet stands); horizontal wheel; kinetic
scrolling; per-window scroll state.
