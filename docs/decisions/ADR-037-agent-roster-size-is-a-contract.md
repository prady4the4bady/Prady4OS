= ADR-037 — the 8-slot agent roster is a contract; growing it is one decision, made once

**Status:** ACCEPTED — governs Section F #66-76 and Section G agent work.
**Date:** 2026-08-14
**Relates to:** DDR-707 (named-agent panel), DDR-713 (card hit-test), DDR-730
(derived liveness), DDR-735/737 (post-mortem metrics, activity pips), DDR-888
(agent DSL in PRISM).

## Why this ADR exists

The Section F/G work queue reads as "add an agent: kernel roster slot, Python
file, smoke gate — pattern of DDR-707/737", repeated ten-plus times. That framing
is wrong, and following it would change a binding UI contract implicitly, once
per agent, with no single place where the decision was made.

**`8` is not an incidental array size. It is load-bearing in four places:**

| site | form |
|---|---|
| `kernel/syscall/sys_aether.c:24` | `#define AGENT_ROSTER_N 8` — backs `g_agent[]`, `sys_agent_roster` clamp, `sys_agent_metrics` |
| `user/compositor.c:42` | `static const char *g_agents[8]` — the literal name table |
| `user/compositor.c:407-411` | `struct agent_metric m[8]`, `nsi(SYS_AGENT_METRICS, …, 8, 0)`, `for (i = 0; i < 8; i++)` |
| `user/compositor.c:425-430` | `agent_card_hit` — "which agent card (0..7)", geometry duplicated from the renderer |

Ring-3 also hard-codes the count at the syscall boundary (`nsi(..., 8, 0)`), so a
kernel-only change is silently ignored by the panel, and a panel-only change
reads past what the kernel fills.

## The layout is bounded, and the bound is not documented anywhere

`render_agent_panel` places card *i* at `y = 70 + i*44`, height 36, so slot *i*
occupies up to `y = 106 + 44i`. There is **no scroll, no pagination, and no
height guard** — the only guard is `if (g_fi.width < 220) return;`, which is
about width. At a 768-line framebuffer the last fully-visible card is i=15; at
600 lines it is i=11. Beyond that, cards render off-screen and
`agent_card_hit` returns indices the user cannot click — a silent UI failure, not
a crash, which is the kind that survives gates.

**Therefore: any roster growth MUST ship with a height guard**, deriving the
drawable count from `g_fi.height` rather than assuming the framebuffer is large
enough.

## Decision

1. **`AGENT_ROSTER_N` may be changed only by a superseding ADR**, never as a
   side effect of adding an agent. This ADR is the one place that decision lives.
2. **All four sites above change together, atomically, in one commit.** The
   compositor must derive its loop bound and its syscall count from a single
   shared constant, not from a repeated literal `8`. A change that updates the
   kernel and leaves `nsi(..., 8, 0)` in place is a silent truncation.
3. **`agent_card_hit` must not re-derive the geometry.** It already duplicates
   the renderer's layout in a comment ("Mirrors the layout in
   render_agent_panel"). Two copies of the same arithmetic is how DDR-910's
   pixel-assumption defect happened. Factor the rectangle computation so the
   renderer and the hit-test cannot disagree.
4. **Growth ships with a derived height guard** (see above), plus a gate that
   fails if a declared slot is not clickable at the gate's framebuffer size.
5. **Until this ADR is superseded, the roster stays at 8.** Section F/G agents
   beyond the existing eight names are implemented as *Python-layer agents
   without a kernel roster slot*, which needs no kernel or compositor change and
   no UI contract change at all.

## Consequence for the F/G queue — this is the point

Most of F#66-76 does **not** need a roster slot. `aether/agents/` already holds
36 modules, essentially none of which own a named UI card. The named eight
(KRYOS..SOLIN) are a *desktop presentation* of a small curated set, not the
registry of every agent that exists. Treating "add an agent" as "add a card"
conflates the two and inflates ten cheap Python items into ten kernel+UI
changes.

So: implement F/G agents in the Python layer with their own gates. Reopen this
ADR only if a specific agent genuinely needs to be one of the eight faces on the
sovereign desktop — and if so, say which existing name it replaces, or supersede
this ADR with the grown size, the height guard, and the factored geometry.

## Not decided here

What the eight named agents *are*. Renaming or repurposing a slot is a DDR-707
question, not this ADR's — this ADR governs only the count and the invariants
that must hold when the count changes.
