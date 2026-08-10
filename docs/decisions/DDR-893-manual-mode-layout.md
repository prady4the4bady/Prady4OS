= DDR-893 — MANUAL MODE is a different desktop (Group 7 item 39)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `user/compositor.c`, `smoke-compositor`.

## What was there

```c
draw_str(mode ? "SOVEREIGN MODE" : "MANUAL MODE", ...)
```

One string. Everything else — gradient, particles, backdrop, agent panel — was
identical in both modes. The item says a mode flag on the Sovereign layout is not
the feature, and it is right.

## Why the two modes are structurally different, not restyled

They answer different questions.

**Sovereign** asks *what are my agents doing* — an ambient gradient, a particle
field, glass cards for eight agents. Ambience is the point.

**Manual** says *let me drive*. A user driving wants chrome where a traditional
desktop puts it, and does not want a particle pass and a full-screen per-row
gradient fill running every frame behind the window they are trying to read.

So Manual is:

- **Flat background.** One fill. No gradient, no particles, no backdrop — those
  are Sovereign's ambient language.
- **A taskbar along the bottom**, with a start button and window buttons: the
  conventional position, the conventional affordance.
- **A menu bar along the top**, in place of Sovereign's accent stripe.
- **No agent panel.** Agents keep running; Manual simply does not put them on
  screen, because that panel is the Sovereign answer to the Sovereign question.

The two paths share only `put_px`/`fill_rect`/`draw_str` — the primitives.
Sharing the *layout* is precisely the design the item rules out.

## The gate asserts STRUCTURE, not a title

```
PRADYOS_MANUAL_TASKBAR_OK h=28
PRADYOS_MANUAL_MENUBAR_OK h=18
PRADYOS_MANUAL_NO_AGENT_PANEL
```

A title string is something *either* layout could print, so asserting on
"MANUAL MODE" would pass for the exact mode-flag implementation this item
replaces. The sentinels name components that only the Manual path emits.

`PRADYOS_MANUAL_NO_AGENT_PANEL` is an assertion about an **absence**, which a
gate cannot otherwise see: nothing failing to appear on screen produces no
output. The Manual path states it positively so the gate can require it.

| Mutation | Applied? | Result |
|---|---|---|
| `if (0)` on the Manual branch — MANUAL falls back to the Sovereign layout | verified yes | **killed** |

That mutation is exactly the pre-DDR-893 behaviour, so the gate now rejects the
implementation this item was written to replace.

## Scope

**Not implemented:** real window management in the taskbar (the buttons are drawn
from a fixed count, not from the live surface table), a functioning start menu,
per-mode input routing, and persisting the chosen mode across boots.

The taskbar-to-surface-table wiring is the one worth naming: `surface_table`
already exists and the compositor already composites client surfaces, so this is
a real follow-up rather than a redesign — the buttons should enumerate live
surfaces instead of drawing four placeholders.

**Group 7 item 39 complete for the layout; live window enumeration named as the
follow-up.**
