# DDR-704 — In-house sovereign-desktop compositor (Layer 7)

> DDR before code, per the brief. First slice that *renders* the PRADYOS desktop.
> **Not wlroots / not Wayland** — a single full-screen ring-3 compositor built on
> the existing `SYS_FB_*` (DDR-702) and `SYS_INPUT_POLL` (DDR-703) syscalls. It
> proves the draw + mode + input pipeline end to end, visually.

## Decisions

### D1 — One full-screen process, direct framebuffer rendering
`user/compositor.c` (musl, ring 3): `SYS_FB_INFO` for geometry, `SYS_FB_MAP` for
the front buffer, then a draw+input loop. No per-client surfaces, no IPC, no
double buffering yet (deferred) — a single owner drawing the whole screen and
presenting with `SYS_FB_FLUSH`. It is spawned with **CAP_SOVEREIGN** (like the
AETHER daemon) so it may flip the mode via `SYS_SET_MODE`.

### D2 — Embedded 8×8 bitmap font (no font file)
A small `8×8` glyph table is encoded directly as a C byte array, covering the
characters the mode labels need (`A D E G I L M N O R S U V` + space). A
`draw_char`/`draw_str` blits glyphs at an integer scale (3× → ~24 px) into the
mapped framebuffer. A real proportional font / fontconfig is a later slice.

### D3 — Two visually-distinct modes (brief §1 palette, simplified)
The loop renders the current mode (read via `SYS_GET_MODE`):
- **SOVEREIGN:** dark background `0x0A0A1A`, purple accent `0x6B21A8`; an accent
  bar + the label `SOVEREIGN MODE` top-left.
- **MANUAL:** lighter background `0x1A1A2E`, teal accent `0x0D9488`; label
  `MANUAL MODE` top-left.

Colors are written BGRA into the framebuffer. The full glass/OKLab ambiance
spec (§1/§2) is deferred — this slice establishes the live mode-driven render.

### D4 — Keyboard drives the mode (and exit)
Each loop iteration drains `SYS_INPUT_POLL`:
- `s` → `SYS_SET_MODE(1)` (SOVEREIGN), `m` → `SYS_SET_MODE(0)` (MANUAL); on a
  change it re-renders and, after confirming via `SYS_GET_MODE`, prints
  **`PRADYOS_COMPOSITOR_MODE SOVEREIGN|MANUAL`** to serial.
- `q` → exit the compositor cleanly.

It prints **`PRADYOS_COMPOSITOR_OK`** after the first frame is drawn + flushed.

### D5 — Distinct sentinels (don't collide with smoke-mode)
The AETHER daemon's boot self-check already prints `PRADYOS_MODE_SOVEREIGN/MANUAL`
(DDR-701, gate `smoke-mode`). To avoid a false pass, the compositor's confirmation
uses the **`PRADYOS_COMPOSITOR_MODE …`** prefix, so `smoke-compositor` greps a
sentinel only the compositor emits — genuinely proving the keyboard→mode→render→
confirm round-trip (a deliberate, documented deviation from the literal sentinel
name in the slice request, for test correctness).

## Gate

`smoke-compositor` (CI, `QEMU_GPU=1`): boot; wait for `PRADYOS_COMPOSITOR_OK`; then
the input harness injects `m` then `s` via QEMU `sendkey` (real IRQ1); the gate
waits for `PRADYOS_COMPOSITOR_MODE SOVEREIGN` — i.e. a real sovereign→manual→
sovereign round-trip driven entirely from the keyboard, rendered to the GPU
framebuffer. Headless QEMU still ACKs the 2D present, so it is verifiable without a
display.

## Non-goals (later slices)
Double buffering / vsync; glass blur + OKLab ambiance transitions; proportional
fonts; the animated 300 ms toggle; per-client surfaces + a draw-command IPC
protocol; mouse/pointer (virtio-input); the named-agent panels. This is the single
full-screen, mode-aware, keyboard-driven desktop those build on.
