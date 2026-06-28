# DDR-706 — Per-client surfaces + compositing (Layer 7)

> DDR before code, per the brief. The compositor (DDR-704) currently draws the
> whole screen itself. This slice adds **client windows**: a ring-3 process renders
> into its own surface buffer and asks the compositor to composite it onto the
> screen — the foundation for app windows and the named-agent panels. Still **not
> wlroots/Wayland** — a minimal in-house shared-surface model.

## Decisions

### D1 — Kernel-owned surface buffers, shared by physical mapping
A client surface is a kernel-allocated, physically-contiguous PMM buffer that the
kernel maps into **both** the client (to draw) and the compositor (to read) — the
same shared-physical-page model as `SYS_FB_MAP` (DDR-702). No copy: the compositor
composites straight from the client's pixels. A fixed kernel table of **16
surfaces**; each `{ phys, w, h, owner_pid, dst_x, dst_y, used, committed }`.

### D2 — Four append-only syscalls (48–51)
```
SYS_SURFACE_CREATE(w, h)              -> surface_id(>=0) | -errno
SYS_SURFACE_MAP(id)                  -> user VA | -errno    (map into the caller)
SYS_SURFACE_COMMIT(id, dst_x, dst_y) -> 0 | -errno          (mark visible at x,y)
SYS_SURFACE_POLL(struct surface_info __user *, max) -> count   (compositor)
```
`surface_info = { u32 id, w, h; i32 x, y; }`. Surfaces are **BGRA**, max 512×512
(bounds the per-surface buffer to ≤1 MiB). `SYS_SURFACE_MAP` places the surface at
a fixed per-id VA — `0x8600000000 + id * 0x100000` — inside the user range, below
the framebuffer mapping (`0x8700000000`) and the mmap arena, `VMM_USER|RW|NX`. A
client maps its own surface to draw; the compositor maps each surface it polls to
read. `SYS_SURFACE_POLL` returns the currently-committed surfaces (id/geometry).

### D3 — Compositor owns the screen, composites committed surfaces
The compositor's loop polls `SYS_SURFACE_POLL`; when the committed set changes it
re-renders the desktop background (mode-dependent, DDR-704), **blits each committed
client surface** at its `(dst_x, dst_y)` (clipped to screen), draws the cursor, and
presents (`SYS_FB_FLUSH`). It prints `PRADYOS_SURFACE_OK <id>` when it first
composites a client surface. Event-driven (re-composite only on change) — no
continuous flushing.

### D4 — Ownership + lifetime
`COMMIT`/`MAP` require the surface's `owner_pid == caller` (a client only touches
its own surfaces); `SYS_SURFACE_POLL` is open (the compositor reads geometry only,
never another client's authority). Surfaces are freed when the owner exits (the
process-teardown path drops them) or on an explicit destroy (deferred). The kernel
never hands a raw kernel pointer to ring 3 — only the mapped VA + the surface id.

## Gate

`smoke-surface` (CI, `QEMU_GPU=1`): a ring-3 client (`user/surfacetest.c`) creates a
64×64 surface, maps it, fills it, and commits it at (100,100) — printing
`PRADYOS_SURFACE_CLIENT_OK`. The compositor polls, composites it onto the desktop,
and prints `PRADYOS_SURFACE_OK 0`; the gate greps that. Proves the full
client-render → commit → compositor-composite → present path.

## Non-goals (later)
Surface resize/destroy, alpha blending / damage rectangles, z-order / focus, input
routing to the focused surface, a real wl_surface protocol, and GPU-accelerated
blits — all deferred. This is the minimal shared-surface compositing the agent
panels and app windows build on.
