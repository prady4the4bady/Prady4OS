# DDR-708 — Surface z-order, focus, and input routing (Layer 7)

> DDR before code, per the brief. Surfaces (DDR-706) currently have no stacking
> order and no focus, and keyboard input is global. This slice adds **z-order**
> (overlapping windows stack), **focus** (one focused surface), and **input
> routing** (keys go to the focused window's client). Foundation for real windowing.

## Decisions

### D1 — Per-surface z-order + focus (kernel)
The kernel surface struct (`kernel/syscall/sys_surface.c`) gains `int32_t z` and
`uint8_t focused`, plus a small **per-surface key ring** (32 bytes). A monotonic
`g_z_top` counter assigns stacking order. `SYS_SURFACE_POLL` returns surfaces
**sorted by z ascending** and includes `z` + `focused` in `surface_info`, so the
compositor composites back-to-front and knows the focused window.

### D2 — Three new syscalls (54–56)
```
SYS_SURFACE_RAISE(id)        -> 0   raise to top + focus it (clears others' focus) [54]
SYS_SURFACE_SENDKEY(id, ch)  -> 0   push a key into the surface's ring (compositor) [55]
SYS_SURFACE_GETKEY(id)       -> ch | -1   pop a key from the surface's ring (owner)  [56]
```
`RAISE` is owner-checked (a client raises/focuses its own window) — and the
compositor may also call it on a click. `SENDKEY` is the compositor forwarding a
keystroke to the focused window; `GETKEY` is the owning client draining its keys.
A surface's key ring is private to its owner (`GETKEY` owner-checked).

### D3 — Compositor: z-ordered compositing + key forwarding
The compositor composites the polled surfaces **in z-order** (POLL is pre-sorted)
so a raised window is on top, and prints the composite order
(`PRADYOS_ZORDER <id>…`). It reads the global keyboard (`SYS_INPUT_POLL`); its own
shortcuts (`s`/`m`/`q`) it still handles, **all other keys it forwards to the
focused surface** via `SYS_SURFACE_SENDKEY`. It prints `PRADYOS_FOCUS id=<id>` when
it first observes a focused surface. (Click-to-focus — focus the topmost surface
under the pointer on a mouse-down — is added too; the gate drives focus via
client `RAISE` for determinism.)

### D4 — Input routing model
Keyboard input stays a single global stream the **compositor arbitrates** (it is
the trusted UI process); routing is the compositor forwarding to the focused
window's private key ring — no shared-ring race between clients, and a client only
ever reads its **own** surface's keys. (Kernel-enforced per-process input queues
keyed on a focus pid are a heavier model, deferred.)

## Gate

`smoke-focus` (CI, `QEMU_GPU=1`): the client (`user/surfacetest.c`) creates **two**
overlapping surfaces (A green, B blue), commits both, and `RAISE`s **B** (top +
focused). The compositor composites them in z-order (B over A, `PRADYOS_ZORDER`)
and prints `PRADYOS_FOCUS id=<B>`. The harness injects a key via QEMU `sendkey`;
the compositor forwards it to B, whose client prints `PRADYOS_FOCUS_KEY id=<B>
ch=<c>`. The gate greps `PRADYOS_FOCUS id=` + `PRADYOS_FOCUS_KEY`.

## Non-goals (later)
Click-to-focus already wired but not gated; drag/move windows; keyboard focus
cycling (alt-tab); per-window decorations / close buttons; pointer enter/leave
events; kernel-enforced focus-pid input gating; copy/paste. Deferred. This is
stacking + focus + keys-to-the-focused-window.
