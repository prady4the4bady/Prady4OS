# DDR-712 — Glass panels + particle field (Layer 7)

> DDR before code, per the brief. The desktop so far is flat: an ambiance bg, an
> accent bar, a mode label, solid agent cards, and windows. The brief's signature
> look (§1, §9 — "depth is information, glass is earned") calls for a **particle
> field** over the background and **frosted-glass** panels. This slice adds both to
> the in-house software compositor (`user/compositor.c`), within what a CPU BGRA
> framebuffer can do — real Gaussian blur stays deferred (the brief itself says
> *don't animate blur radius; crossfade pre-baked states*).

## Decisions

### D1 — Per-ambiance particle field (brief §1)
A particle layer is drawn **after** the ambiance bg + accent bar and **before** the
windows, so windows and the mode label stay on top (smoke-compositor's label and
smoke-agents' cards are unaffected). Particles are a fixed pool with
deterministic positions — **no `Math.random`/libc rand**; positions come from a
small integer LCG seeded by a compile-time constant, so every boot is identical
and the gate is reproducible. Each ambiance picks a profile from the brief:

| Ambiance | Particles | Colour | Motion |
|----------|-----------|--------|--------|
| DAWN  | 120 motes  | lavender→coral | slow drift up-right 0.1–0.3 px/f |
| DAY   | 0 (3-node mesh, **deferred**) | — | none |
| DUSK  | 60 embers  | `#FF6B35`→`#FFB800` | rising 0.05–0.2 px/f |
| NIGHT | 200 stars  | white, 4 bright w/glow | twinkle (op cycles), nebula drift deferred |

Counts are the brief's; a single `put_px` per particle per frame is trivial for the
software compositor. Particles wrap at the screen edges. The current ambiance index
selects the profile (the compositor already tracks `g_cur_amb`). The field is drawn
by a new `render_particles()` called from `render()`.

### D2 — Frosted-glass panels (brief §9)
"Glass" is approximated by **alpha compositing over the live background**, not a
real blur (deferred): a glass rect samples each underlying pixel and blends it
toward a translucent tint (`rgba(255,255,255,~0.08)`) plus a 1px accent border —
the brief's `bg rgba(255,255,255,0.06)` + border, minus the blur. A new
`blend_px(x,y, r,g,b, alpha)` helper does the per-pixel `out = src*(1-a)+tint*a`
composite (the existing `put_px` is opaque). The **agent cards** (DDR-707) are
re-rendered as glass: the active/inactive status dots stay solid and unchanged, so
**smoke-agents is unaffected**. Per §9 rule 2 (max 3 glass layers) only the cards
are glass this slice.

### D3 — Where the work runs (no per-frame spin)
`render()` already runs on every state change (mode flip, ambiance transition,
surface set/focus change, drag). Particles are drawn as part of `render()`, so they
appear and update whenever the scene is recomposited — **no new always-on present
loop** (that would hammer the GPU control queue and risk the other L7 gates'
timing). The particle phase advances by a frame counter incremented each `render()`.
First render emits the new sentinels once:
`PRADYOS_PARTICLES_OK n=<count>` and `PRADYOS_GLASS_OK`.

## Gate
`smoke-visual` (CI, `QEMU_GPU=1`, client-driven — no QMP): boot and grep
`PRADYOS_PARTICLES_OK` + `PRADYOS_GLASS_OK` (the compositor, emitted on its first
desktop render). 44 CI gates total. All existing L7 gates
(compositor/mouse/surface/agents/ambiance/drag/focus/winops) keep passing — the
mode label, agent dots, and window compositing are drawn on top of the particle
layer and the glass tint preserves card legibility.

## Non-goals (later)
Real Gaussian/box blur + saturation passthrough; the DAY 3-node mesh + horizon
SVG; DUSK sun-bloom radial + NIGHT nebula radials and per-star 4px glow bloom;
multi-stop gradients; the 60 s particle crossfade between ambiances; glass on the
dock/right-panel/modal (only the agent cards are glass here). Deferred — this slice
is the particle field + frosted-glass cards.
