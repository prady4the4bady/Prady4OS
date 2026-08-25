# DDR-998 — `SURF_EV_CLOSE`: let the owner save state before a forced close

**Status:** DESIGN.
**Extends:** DDR-711 (`SYS_SURFACE_CLOSE`), DDR-715 (close box), DDR-718 (event
ring), DDR-729 (surface lifecycle), DDR-911 (the type-3 composited event).
**Gate:** `smoke-surfclose` (new).

---

## 1. What happens today

`user/compositor.c` handles a close-box click by calling `SYS_SURFACE_CLOSE`
immediately. The kernel captures the slot's frames, clears `used`, unmaps the
owner's view, and returns the pages to the buddy allocator — all before the owner
runs another instruction. The owner is never told, gets no chance to flush state,
and if it touches its (now unmapped) surface it takes a clean user-kill.

That is correct as a *policy* — authority over the desktop stays with the
sovereign compositor (DDR-715) — but it is missing the *courtesy* half: ask
first, force second.

## 2. Event type 4, not 2

The obvious number is wrong. `SURFACE_EQ` events already use:

| type | meaning | DDR |
|---|---|---|
| 1 | `RESIZE_REQ(w,h)` | DDR-718 |
| 2 | `SCROLL(delta)` | DDR-725 |
| 3 | `COMPOSITED` | DDR-911 |

so `SURF_EV_CLOSE` is **type 4**. Writing this down because the header comment on
`struct surf_event` in `sys_surface.c:30` still says only `type 1 = RESIZE_REQ`,
and reading that comment rather than the clients is how a duplicate gets shipped.
Fix the comment in the same commit.

## 3. The ask-then-force protocol

On a close-box click the compositor:

1. sends `SURF_EV_CLOSE` (type 4) to the surface,
2. records `(id, gen, deadline)` in a small pending table,
3. prints `PRADYOS_WM_CLOSE_REQ id=N`,

and keeps running. Each frame it retires pending entries:

- **surface gone from `SURFACE_POLL`** → the owner closed itself. Print
  `PRADYOS_WM_CLOSE id=N owner=1`, drop the entry.
- **still present, `gen` matches, deadline passed** → force. Call
  `SYS_SURFACE_CLOSE`, print `PRADYOS_WM_CLOSE id=N owner=0`, drop the entry.
- **still present, `gen` DIFFERS** → the slot was recycled under us. Drop the
  entry **silently and without closing anything** (§4).

`PRADYOS_WM_CLOSE id=N` keeps its exact existing prefix so `smoke-wmclose`'s
`grep -q "PRADYOS_WM_CLOSE id=2"` is untouched; `owner=` is appended.

## 4. The recycle trap, and why a generation counter is required

A surface id is a slot index into a 16-entry table, and `surf_take_free` returns
the slot to the pool immediately. So between "send the close request" and "the
deadline expires" the owner can close, the slot can be reused by a **different
process**, and the compositor's deadline force-close would then destroy an
innocent new window that merely inherited the number.

This is not hypothetical bookkeeping: it is the same identity-versus-address
mistake §NON-NEGOTIABLE 18 warns about, one level up. An id does not identify a
surface when every surface is drawn from the same 16 slots.

`struct surface` therefore gains `uint32_t gen`, incremented on every allocation,
and `struct surface_info` exposes it. The compositor force-closes only when the
generation it recorded still matches.

**`struct surface_info` is declared TWICE** — `kernel/syscall/sys_surface.c:46`
and `user/compositor.c:43` — and `SYS_SURFACE_POLL` copies out
`count * sizeof(struct surface_info)` into the caller's array. If the two
declarations disagree in size the kernel overruns the compositor's stack buffer.
Both must change in the same commit. This is §INV.13's `PT_HI` lesson in a
different file, and it earns its own line here because the failure mode is silent
memory corruption rather than a build error.

## 5. The deadline

Two clocks, and the grace expires only when BOTH have passed:

- `wall_secs()` (`SYS_CLOCK`, one-second resolution) — `>= CLOSE_GRACE_SECS`.
- compositor frames since the request — `>= CLOSE_GRACE_FRAMES`.

Seconds alone are not enough because the resolution is one second: a click landing
just before a boundary would grant a grace of nearly zero. Frames alone are not
enough because the frame rate is a scheduling-dependent quantity, which is exactly
what DDR-911 removed from this file after item 16 changed it. Requiring both gives
a floor that neither clock can collapse on its own.

Both numbers are to be **measured** in the gate, not asserted: the gate prints how
many frames and seconds the owner actually needed, so the margin is a number in a
log rather than a belief. §NON-NEGOTIABLE 17 — the denominator is the frame rate,
which the gate must also report.

## 6. The owner side

`user/surfacetest.c` drains type 4 on window A, prints `PRADYOS_SURF_SAVED id=N`
(standing in for "flush state"), then calls `SYS_SURFACE_CLOSE` itself. Ordering
is the assertion: SAVED must appear BEFORE the close, or the courtesy did not
happen.

## 7. Gate — `smoke-surfclose`

Two arms in one boot, both driven from the published `close=` handle (§INV.5):

- **Arm A (owner honours)** — click A's close box. Assert, in order:
  `PRADYOS_WM_CLOSE_REQ id=A`, `PRADYOS_SURF_SAVED id=A`,
  `PRADYOS_WM_CLOSE id=A owner=1`. Order is checked by line index, not by
  presence — three lines in the wrong order is a different system.
- **Arm B (owner wedged)** — a surface whose owner never drains type 4 must still
  die. Assert `PRADYOS_WM_CLOSE id=B owner=0` within the deadline.

## 8. Mutations (required)

- **M1** — compositor force-closes immediately, ignoring the grace. Arm A must
  fail on ORDER (`WM_CLOSE` before `SURF_SAVED`), not merely on presence.
- **M2** — drop the deadline entirely (ask, never force). Arm B must hang and
  fail; arm A must still pass, proving the two arms test different halves.
- **M3** — force-close without the generation check. Needs a recycle inside the
  window to observe, so if it cannot be produced deterministically this is
  recorded as UNMEASURED with the reason, not claimed.

## 9. NOT in scope

- No "unsaved changes" dialog, no cancel: the owner may delay within the grace,
  never veto. Sovereign authority is unchanged.
- No per-surface grace negotiation — one constant, imposed, as DDR-718 does
  with the 32 px floor.
- The kernel does not enforce the deadline. Force-close authority already lives
  in the compositor (DDR-715) and splitting it across two layers would give two
  places to get the recycle check wrong.
