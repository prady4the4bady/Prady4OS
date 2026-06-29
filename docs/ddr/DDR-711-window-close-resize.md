# DDR-711 — Window close + resize (Layer 7)

> DDR before code, per the brief. DDR-706/708/710 gave windows that can be
> created, committed, stacked, focused, and dragged. They cannot yet be **closed**
> (freed) or **resized**. This slice adds both as compositor/owner-driven surface
> operations, completing the basic window lifecycle (create → draw → move →
> resize → close).

## Decisions

### D1 — `SYS_SURFACE_CLOSE(id) -> 0 | -errno`  [59]
Destroys a surface and frees its kernel-owned PMM buffer. Permitted for the
surface **owner** OR a `CAP_SOVEREIGN` caller (the compositor), mirroring
`SYS_SURFACE_MOVE`/`RAISE`.

- When the **owner** closes (the common path: `current_thread->cr3` *is* the
  owner's address space), the surface's VA range
  (`SURFACE_VA_BASE + id*SURFACE_VA_SLOT`, `npages` pages) is unmapped from the
  active address space with `vmm_unmap`, so a stale read after close faults →
  clean user-kill rather than reading freed frames (W^X / ADR-021 isolation
  upheld). A `CAP_SOVEREIGN` caller closing *another* process's surface frees the
  frames and clears the slot but cannot unmap the owner's tables (they are not the
  active AS); the owner must not touch a sovereign-closed surface. Documented
  limitation, not used by the gate.
- The PMM buffer is returned with `pmm_free_pages(phys, order_for(npages))` — the
  same buddy order `sys_surface_create` allocated. The slot is zeroed (`used=0`),
  so `SYS_SURFACE_POLL` stops listing it and the id is reusable.

### D2 — `SYS_SURFACE_RESIZE(id, w, h) -> 0 | -errno`  [60]
Changes a surface's pixel dimensions. **Owner-only** (it owns the buffer it draws
into — a sovereign resize of someone else's window is out of scope). `w`/`h` are
validated against `SURFACE_DIM_MAX` (1..512) exactly like create.

A resize allocates a **fresh** PMM block for the new size, zeroes it, unmaps the
owner's old VA pages from the active AS, frees the old block, and points the
surface at the new buffer with updated `w/h/npages`. Position (`x,y`), stacking
(`z`), focus, and `committed` are **preserved** — the window stays where it is at
its new size. The owner then re-maps (`SYS_SURFACE_MAP`, same base VA) and redraws
into the new buffer. The compositor reads the new `w/h` from `SURFACE_POLL` and the
new pixels from its per-frame `SYS_SURFACE_CMAP` — no compositor change needed for
resize beyond D3.

### D3 — Compositor recomposites on surface-set **shrink**
The existing recomposite trigger was `ns > composited` (grew) or focus change — it
never fired when the live-surface count *dropped*. Closing a window shrinks the
set, so the trigger becomes `ns != composited`. On a shrink the compositor repaints
the desktop (which erases the closed window, since `render()` clears to the
ambiance bg before blitting only live surfaces) and prints `PRADYOS_SURFACE_GONE
n=<ns>`. Resize is covered by the owner's post-resize `RAISE` (a focus change,
already a trigger).

### D4 — Test driver (`user/surfacetest.c`, extended)
`surfacetest` already creates windows A (id 0) and B (id 1, raised/focused) for
`smoke-surface`/`smoke-focus`/`smoke-drag` and must keep doing so unchanged. This
slice adds a **third** window C (id 2) used only for close/resize, placed off to
the side (`420,70`) so it never sits under B's title bar, and **never raised** so B
keeps focus — leaving the A/B gates untouched:
1. create + commit C (red, 64×64) → `RESIZE` C to 96×96 → re-map + redraw → print
   `PRADYOS_RESIZE_OK` (up front, before the key loop).
2. in the key-drain loop, after ~2000 ticks (the compositor has long since
   composited C — proven reachable within the gate's 90 s), `CLOSE` C → print
   `PRADYOS_CLOSE_OK`. B's focus is never disturbed.

## Gate
`smoke-winops` (CI, `QEMU_GPU=1`, client-driven — no QMP): boot, and grep
`PRADYOS_RESIZE_OK` + `PRADYOS_CLOSE_OK` (the client) + `PRADYOS_SURFACE_GONE` (the
compositor recompositing after the close shrinks the set). 43 CI gates total.
Existing `smoke-surface`/`smoke-focus`/`smoke-drag` continue to pass on the
unchanged A/B windows.

## Non-goals (later)
Close/min/max **buttons** in the title bar (this is the syscall + client path, not
a clickable chrome control); maximise/restore; resize **handles**/edge-drag with
the pointer; snapping/tiling; per-window title strings. Deferred. This is the
close + resize surface operations.
