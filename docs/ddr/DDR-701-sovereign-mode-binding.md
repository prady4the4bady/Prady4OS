# DDR-701 — Sovereign/Manual mode binding (Layer 7 §3, data layer)

> Per the Layer-7 brief, every slice needs a DDR **before** its code. This is the
> first Layer-7 slice. It binds the brief's most critical UI element — the
> Sovereign/Manual toggle (§3) — to the kernel, at the **data/control layer**,
> ahead of the visual compositor.

## Context / honest blocker (record per CLAUDE.md §3)

The Layer-7 brief specifies a **Wayland compositor + desktop shell** (wlroots
scene graph, GPU framebuffer, OKLab transitions, glass panels). Its stated
prerequisite is a **GPU framebuffer + `/dev/tty0`** ("Layer 5 complete"). PRADYOS
today has **no GPU framebuffer driver** — output is the serial console + text-mode
VGA (`kvga_line`). The mandated build order (§12: 7a wlroots skeleton → …) cannot
begin until a VirtIO-GPU framebuffer + modesetting driver exists.

**BLOCKER (visual compositor):** Layer-7 §12 (7a–7j) is blocked on a VirtIO-GPU
framebuffer driver (double-buffered, modeset), which needs its own ADR + slice
sequence (a Layer-7 "slice 0"). This DDR does **not** attempt the compositor; it
delivers the toggle's kernel binding so the future shell is a thin renderer over
an already-proven control surface.

## Decision — bind the toggle to the existing mode NSI

The toggle's two states map directly to the AETHER mode flag (ADR-026 D2),
already exposed by `SYS_GET_MODE` (29) / `SYS_SET_MODE` (30):
- **SOVEREIGN** = `g_sovereign_mode == 1` (agents auto-approved).
- **MANUAL**    = `g_sovereign_mode == 0` (actions await approval).

Two ring-3 surfaces, matching the security model:
1. **Query (unprivileged):** PRISM gains a `mode` builtin — `mode` / `mode get`
   prints `MODE: SOVEREIGN|MANUAL` via `SYS_GET_MODE`. `mode set sovereign|manual`
   attempts `SYS_SET_MODE`; from PRISM (no `CAP_SOVEREIGN`) this returns `-EPERM`,
   demonstrating that flipping the toggle requires sovereign authority — exactly
   what the future shell's privileged compositor will hold. This is the
   serial-console stand-in for the brief's Super+M toggle.
2. **Authoritative toggle:** the AETHER daemon (`CAP_SOVEREIGN`) performs the real
   flip. At startup it runs a binding self-check —
   `GET`(sovereign) → `SET manual` → `GET`(manual) → `SET sovereign` → `GET`
   (sovereign) — emitting `PRADYOS_MODE_SOVEREIGN`, `PRADYOS_MODE_MANUAL`, and
   `PRADYOS_MODE_TOGGLE_OK`, then proceeds (ending in **sovereign**, so the agent
   pipeline / `smoke-aether` is unaffected).

## Gate

`smoke-mode` (CI): boot, the daemon's binding self-check toggles the mode and the
serial shows `PRADYOS_MODE_TOGGLE_OK` (and both state sentinels). This proves the
toggle's control path end-to-end from the sovereign authority, and that
`SYS_SET_MODE` honors `CAP_SOVEREIGN`. PRISM's `mode get` is exercised manually /
by the existing shell surface.

## Non-goals (deferred to the compositor slices, post-GPU-framebuffer)
The 300ms spring animation, knob overshoot, radial ripple, glass pill, crown/
monitor glyphs, OKLab accent shift, Super+M keybinding — all the **visual**
toggle (§3) — wait on the GPU framebuffer + wlroots stack. This slice is purely
the binding they will render.
