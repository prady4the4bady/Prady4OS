# DDR-729 — Surface destroy: lifecycle completion + one-owner reclamation

**Status:** proposed (pre-code)
**Layer:** 7 (desktop) / cross-cut with mm + proc
**Supersedes:** nothing. Completes the surface lifecycle opened by DDR-706 and
extended by DDR-711 (close/resize), DDR-708 (z/focus), DDR-718 (event channel).

## Problem

A surface (`kernel/syscall/sys_surface.c`) is a kernel-owned, physically
contiguous PMM buffer (`pmm_alloc_pages(order)`) that the kernel maps into the
owning client (draw) and the compositor (read). The table is bounded at 16
slots. Teardown today is **explicit only** — `SYS_SURFACE_CLOSE` (DDR-711). Two
root defects follow:

1. **Leak on exit (reachable).** A client that exits — normally, via signal, or
   via a fault-kill — without calling `SYS_SURFACE_CLOSE` leaks its slot
   (`used` stays 1) and its PMM frames forever. With only 16 slots, a handful of
   create/exit cycles exhausts the table (`-EMFILE`). Nothing in `sched_exit`
   reclaims surfaces.

2. **Double-free / ambiguous ownership (latent, corrupting).** Surface frames
   are mapped into the client with `VMM_USER|VMM_RW|VMM_NX` — **without**
   `PTE_SW_SHARED`. `vmm_destroy_address_space` → `free_subtree` therefore
   treats those leaf pages as private user data and `ptnode_free`s them when the
   client's address space is destroyed. So the frames have **two owners**: the
   surface layer (which `pmm_free_pages(phys, order)`s them on close) and the
   address-space teardown (which `ptnode_free`s each 4 KiB page). This already
   mis-frees today in two paths: (a) a client that exits with a live mapping;
   (b) `SYS_SURFACE_CLOSE` invoked by the **sovereign** compositor on another
   process's surface — it frees the frames but does not unmap the owner's
   (inactive) AS, so the later `vmm_destroy_address_space` of that owner frees
   them a second time. The buddy order/ptnode mismatch corrupts allocator
   accounting.

The FB mapping (`kernel/syscall/sys_fb.c`, DDR-702) has the **identical** shared
bit omission — the GPU scanout frames are kernel/GPU-owned but mapped into the
client without `PTE_SW_SHARED`. It has not bitten only because the sole FB
consumer is the never-exiting compositor. It is the same root and is fixed here.

## Decision — one owner, one free point

**Ownership invariant.** Surface pixel frames (and the FB scanout frames) are
owned **solely by the layer that allocated them** (`g_surf[id].phys` at buddy
order `order_for(npages)`; the GPU resource for the FB). Every client/compositor
mapping is a **view**, marked `PTE_SW_SHARED` exactly like the vDSO
(`kernel/vdso/vdso_page.c`). Consequences:

- `free_subtree` skips shared leaves, so **address-space teardown never frees a
  surface/FB frame.** This removes the second owner — the linchpin fix. It makes
  every teardown path (owner close, sovereign close, exit reap) single-free by
  construction.

**Teardown triggers (the single teardown path, `surface_free_slot`).**
- Explicit: `SYS_SURFACE_CLOSE` (unchanged surface API).
- Automatic: `surface_reap_pid(pid)` called from `sched_exit`, freeing every
  slot owned by the exiting pid. No unmap is needed in the reap path — the
  owner's AS is being destroyed and no thread will run in it again; the frames
  are returned to the buddy allocator, and `PTE_SW_SHARED` guarantees the
  subsequent `vmm_destroy_address_space` leaves them alone.

Because pids are monotonic (`next_tid++`, never recycled), a leaked slot cannot
be *adopted* by a pid-recycled process; the defect is purely a leak + the
double-free, both closed above.

**SMP correctness.** `g_surf` is global and, post-rq-3, touched from any CPU
(a client on one CPU, the compositor on another, `sched_exit` on a third). Add a
leaf spinlock `g_surf_lock` (ADR-029/030 discipline) guarding all slot
**metadata**: the free-slot scan+claim in create, the free+clear in
close/resize/reap, and the poll snapshot. Rules that keep the IRQ-off window
short and avoid holding the lock across page-table walks or large memsets:

- **create:** `pmm_alloc_pages` + zero the buffer *before* taking the lock; then
  under the lock claim a free slot and initialise its fields (or, if the table
  is full, drop the lock and `pmm_free_pages` the just-allocated buffer →
  `-EMFILE`).
- **close / resize / reap:** under the lock, snapshot `phys`/`npages`/`owner`
  and mark the slot free (or swap in the resize buffer); drop the lock; then
  `pmm_free_pages` / `vmm_unmap` outside it.
- **map / cmap:** under the lock, validate + snapshot `phys`/`npages` into
  locals; drop the lock; then run the `vmm_map_in` loop on the locals.
- **poll:** build the `surface_info` snapshot under the lock; drop it; `copyout`
  after (never `copyout` under a spinlock).
- **metadata mutators** (commit/move/raise/set_title) and the SP/SC key/event
  rings: take the lock for the brief field/head-tail update. The rings stay
  correct; uniform locking is the single discipline.

The compositor's dangling-view window (frames freed while its long-lived
read-map still resolves) is **unchanged from existing close semantics**: the
compositor re-polls every frame and stops reading a surface the instant it
leaves the poll list. Reap marks `used=0`, so the surface de-lists on the next
poll exactly as an explicit close does. No new hazard.

## Lifecycle invariant (stated precisely)

- **exists**: `g_surf[id].used == 1`. The slot and its frames are live and owned
  by `owner_pid`.
- **active/visible**: `used == 1 && committed == 1`. Appears in `SYS_SURFACE_POLL`.
- **freed**: `used == 0`. Frames returned to the buddy allocator; no view may
  resolve to them as *owned* memory (all views were `PTE_SW_SHARED`, so AS
  teardown ignores them). A freed slot never resurrects — a new surface reusing
  the index is a fresh `create` with a fresh (higher) pid check.

There is exactly **one** function that frees a slot's frames and clears it
(`surface_free_slot`), reached from exactly three callers (owner close, sovereign
close, exit reap). No best-effort cleanup; no hidden resurrection.

## Gate — `smoke-surfdestroy` (75 gates)

A new ring-3 test (`user/surfdestroytest.c`, CAP-less) drives, on `-smp 4` so
the reap runs cross-CPU:

1. **explicit destroy**: create → map → commit → `SYS_SURFACE_CLOSE`; a second
   create must reuse the freed slot (no leak) → `PRADYOS_SURFDESTROY_REUSE_OK`.
2. **exit reclamation**: fork a child that creates+commits N surfaces and exits
   **without** closing them; the parent, after reaping the child, creates N+1
   surfaces successfully (proving the child's slots were reclaimed on exit) →
   `PRADYOS_SURFDESTROY_EXIT_OK`.
3. **churn**: a create/close loop of >16 iterations completes without `-EMFILE`
   → `PRADYOS_SURFDESTROY_CHURN_OK`.

Sentinel `PRADYOS_SURFDESTROY_OK` printed only when all three pass; forbidden
`SURFDESTROY FAIL`. Wired into `Makefile` + `.github/workflows/ci.yml`.

Regression: the full existing surface/compositor set (`smoke-surface`,
`smoke-compositor`, `smoke-winops`, `smoke-focus`, `smoke-drag`, `smoke-visual`)
plus `smoke-vdso` (shared-bit path) and `smoke-fb` (the FB flag change) must stay
green, then the full 75-gate suite.

## Non-goals

- No change to the surface *API* surface (no new syscall) — teardown becomes
  automatic and the ownership model becomes sound; the client-visible calls are
  unchanged.
- No compositor protocol handshake — teardown is compositor-local by the
  re-poll contract; AETHER is not involved.
- wlroots/Wayland remain out-of-tree (the standing wall).
