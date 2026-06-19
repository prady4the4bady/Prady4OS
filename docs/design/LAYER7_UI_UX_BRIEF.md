# PRADYOS Sovereign Edition — UI/UX Master Brief v1.0 (Layer 7)

> Binding design spec for Layer 7 (Wayland compositor + desktop shell). Provided
> by the user 2026-06-18. Prerequisite: Layer 5 complete (GPU framebuffer,
> /dev/tty0). Do not start Layer 7 until Layers 4–6 gates pass. Every Layer-7
> slice needs a DDR in `docs/ddr/DDR-XXX.md` written **before** its code.

## Prime directive
Look like it arrived from 10 years in the future — not cluttered, not a Linux
clone, not macOS-inspired. Default to restraint: one beautiful thing done
perfectly over ten clever things. If unsure whether to add an element, don't.
Self-answer all technical decisions except persisted formats / security policy;
record reasoning in a DDR. **Depth is information, glass is earned, restraint
over spectacle.**

## §1 The four ambiances (continuous, real-time, sun-driven; not a theme toggle)
Driven by system clock + geolocation (UTC offset fallback). Soft boundaries,
transitions begin 15 min before each. Five layers (background, glass, type,
accent, motion) shift together. Colors interpolate in **OKLab** (not sRGB).

- **DAWN 05:00–08:59** "world is waking". BG: aurora filaments indigo `#1A0A2E`→
  lavender `#C8A4E8`→coral `#F4A261`; 120 motes (op 0.15–0.4, drift 0.1–0.3
  px/f). Glass: blur(28px) saturate(180%), bg `rgba(255,255,255,0.06)`, border
  `rgba(255,255,255,0.12)`, inner glow `rgba(200,164,232,0.15)`, shadow
  `0 8px 32px rgba(26,10,46,0.6)`. Type: Inter 300; display "Good morning,
  Sovereign." 40px/200/0.02em `#EDE8F5`; body 14/400. Accent `#C8A4E8`, glow
  `#9B6AC8`@40%. Status dots pulse 0.8s scale 1.0→1.15.
- **DAY 09:00–16:59** "clarity/focus". BG: `#0D1B2A`→`#1B2B4B`→`#2D4A6B`, slow
  3-node mesh (20s), minimal horizon 10%; **no particles**. Glass: blur(20px)
  sat(140%), bg `rgba(255,255,255,0.08)`, border `rgba(100,160,255,0.20)`,
  shadow `0 4px 24px rgba(0,20,60,0.5)`. Type body 450; "Focus mode." / "The
  machine is at your service." `rgba(220,235,255,0.9)`. Accent `#4FAEFF`@30%.
  System-monitor panel slightly prominent.
- **DUSK 17:00–20:59** "warm, earned". BG: `#1A0800`→`#3D1500`→`#6B2D00`, sun
  bloom radial at 85% 90% `rgba(255,120,30,0.25)`; 60 ember motes `#FF6B35`→
  `#FFB800` rising 0.05–0.2 px/f; horizon 25% warm charcoal. Glass: blur(24px)
  sat(200%), bg `rgba(60,20,5,0.45)`, border `rgba(255,140,50,0.25)`, inner
  `rgba(255,120,30,0.08)`, shadow `0 8px 40px rgba(80,20,0,0.7)`. Type body 350;
  "Good evening, Sovereign." `rgba(255,210,160,0.9)`. Accent `#FFB347`, glow
  `#FF6B35`@35%.
- **NIGHT 21:00–04:59 (Sovereign home ground)**. BG: `#000008` base; two nebula
  radials (`#120024`@30%40% r600; `#001220`@70%60% r500); 200 stars op 0.1–0.6,
  4 bright w/4px glow; nebula drift 120s ±20/±10px. Glass: blur(32px) sat(220%),
  bg `rgba(10,5,30,0.55)`, border `rgba(120,80,255,0.20)`, inner
  `rgba(80,40,200,0.06)`, shadow `0 12px 48px rgba(0,0,20,0.8)`; hover border
  `rgba(160,120,255,0.45)` + `0 0 20px rgba(120,80,255,0.3)`. Type: "Good
  evening, Sovereign." `#E8E0FF`; "SOVEREIGN MODE" `#A078FF` 10px/0.35em/600
  uppercase; body `rgba(200,190,255,0.80)`. Accent `#7B4FE0`, active glow
  `#9B6FFF`@50%. Faint scorpion-constellation mascot top-left op 8%.

## §2 Transition engine
CSS custom props on `:root` updated by Rust `ambianced` daemon every 60s. OKLab
lerp(current,target,0.002)/tick, logarithmic easing. Auto change 900s (15 min);
Manual Mode toggle 600ms. Particle field crossfades over 60s (never hard cut).
GPU framebuffer double-buffered, no tearing. Time: `sys_clock_gettime(CLOCK_
REALTIME)` + RTC (Layer 3); fallback PIT monotonic.

## §3 Sovereign/Manual toggle (most critical element)
Top-left sidebar below logo, 180×44 pill. SOVEREIGN (agents running): crown icon
20px accent, "SOVEREIGN" 11/600/0.25em, glowing pill w/night-accent gradient, 3s
breathing pulse. MANUAL (user-controlled): monitor-outline icon, "MANUAL", unlit
track border-only. Animation 300ms `cubic-bezier(0.34,1.56,0.64,1)`: knob spring
overshoot L→R, gradient sweep, side text slides in, radial ripple (op 0.6→0,
r0→80px, 400ms). Shortcut Super+M. Always visible.

## §4 Layout (1440×900 ref; wlroots scene graph; no X11/XWayland unless asked)
- **Left sidebar 200px fixed**: top = scorpion logo 48px + "SOVEREIGN EDITION"
  9px/0.3em; middle = toggle, hairline divider, 80px mode-description card
  (SOVEREIGN "The machine governs. You approve."; MANUAL "Full control. All
  tools. All yours."; crossfade 400ms); bottom = avatar 40px + "Sovereign." +
  status dot.
- **Center (Sovereign)**: top-center greeting; command bar 560×52 @38% height
  ("Ask PRADYOS anything...", left rotating 4-pt spark 0.5rpm, right arrow,
  expands to 760px on focus 300ms spring); floating dock bottom-center 64px pill
  (8 icons + divider + settings + trash; 40→52px hover spring; blur 40px; 32px
  from bottom).
- **Center (Manual)**: canvas fades 200ms, rebuilds 300ms slide-up; top small
  left-aligned search; left Finder-style drawer; center windows/app grid; right
  system panel; full-width floating taskbar.
- **Right panel 280px, collapsible (Super+R)**, 4 glass cards 12px gaps: (1)
  System Overview — 4 donuts CPU/GPU/RAM/Disk 48px stroke3, animate from 0 on
  mount, update 2s tween (source: /proc FS Layer 5); (2) Network — ↓/↑ Gbps +
  60s rx/tx sparkline, area fill 15%; (3) AI Agents — "8 Active" badge, 2×4 grid
  of 64×28 chips (idle dim / active glow pulse / thinking rotating arc): KRYOS
  PRAX LUMYN AHNIS / IRIS RUFLO HERMES SOLIN; (4) Clock — 48px/200 time, date
  12px, avatar 32px.

## §5 Approval popup (Sovereign Gate UI)
Render ≤16ms of kernel posting to NIA bus (Layer 6 ADR-036). Modal-lite,
top-right below clock, slides down 400ms spring. 300×140 glass, accent border;
header = risk badge (HIGH red / LOW teal) + 30s depleting circular countdown;
agent+action 2 lines/13px; APPROVE (solid accent) / REJECT (glass outline) 44px
pills 12px gap. Timeout → shake ±4px×3, red, auto-REJECT, slide up. Multiple
pending stack 8px offset (deck); tap front.

## §6 Icon language — "Sovereign Glyphs"
No flat/Material/SF. Monoline SVG 2px stroke, rounded caps, geometric
abstraction, ambient glow ring when active (4px/40%/accent), 24×24 viewBox,
clean at 16/24/48px. Agents: KRYOS concentric hexagons; PRAX diamond+orbit arcs;
LUMYN open book+spark; AHNIS shield+eye; IRIS aperture iris; RUFLO node flow
graph; HERMES wing+bolt; SOLIN brackets+cursor. System: AI Terminal chevron in
rounded rect; Files folded pages; System Mon waveform; Agent Center 8-node
radial; Projects briefcase; Reports ascending bars. Ship in
`assets/icons/{sovereign,system}/` 1x(24)+2x(48) + `.json` metadata
(name, category, description, accent_color_override|null).

## §7 Typography (bundle all; 8pt grid, multiples of 4px)
Inter Variable 200–700 (OFL); JetBrains Mono 300–600 (Apache2); Outfit Variable
100–400 (OFL, greetings/large numbers only). Display 40/48 Outfit200; H1 28/36
Inter300; H2 20/28 Inter400; Body 14/22 Inter400; Caption 11/16 Inter500/0.15em;
Label 10/14 Inter600/0.25em UPPERCASE; Mono 13/20 JBM400; MonoBold 13/20 JBM600.
`-webkit-font-smoothing:antialiased; text-rendering:optimizeLegibility;
font-feature-settings:"kern"1,"liga"1,"calt"1`.

## §8 Motion classes
A Micro ≤200ms ease-out (hover/focus/press). B Transition 200–400ms
`cubic-bezier(0.34,1.56,0.64,1)` (always for spatial change). C Macro 400–900ms
custom spring (stiffness200 damping25 mass1) via WAAPI/CSS-spring, not ease.
D Ambient ∞ loop (particles/nebula/glow/twinkle) — `will-change:transform`,
compositor thread, rAF 60fps (30fps battery-saver). Rules: never color-only
(pair with opacity/transform); never linear; honor `prefers-reduced-motion`
(A–C → 0ms, D stops); don't animate blur radius — crossfade two pre-baked states.

## §9 Glassmorphism rules (enforced)
1 Glass only over rich/dynamic backgrounds (always true here). 2 Max 3 glass
layers deep (bg→panel→card; no 4th). 3 Every surface passes WCAG AA 4.5:1 body
text (test programmatically; on fail raise border opacity + text-shadow, don't
reduce blur). 4 Blur scales with z: bg(z1)20px, card(z2)28px, modal(z3)36px,
dock 40px. 5 "Reduce Transparency" → all surfaces `rgba(15,10,35,0.92)`, opacity
only, no reflow.

## §10 Reference sources (study before coding)
NNGroup glassmorphism; visionOS spatial guide (think.design); Circadian UI
(dev.to/fabianzimber); shadcn Northern Lights (OKLab); Hyprland (layer-shell,
blur, spring); SwayFX (blur+radius in wlroots); Sunpaper (sun-phase→asset);
awesome-wayland; Dribbble glassmorphism-dark; Behance UI/UX; designmd spatial-ui
("depth is information, glass is earned, restraint over spectacle").

## §11 Tech stack
Compositor: Rust + wlroots (smithay/wlr-rs); protocols wlr-layer-shell (panels/
dock), xdg-shell (windows), wlr-screencopy (IRIS), wlr-virtual-pointer (RUFLO);
render through wlroots scene API only. Shell `pradyos-shell`: GTK4 (libadwaita
off, custom CSS) — chosen for text/a11y/blur-passthrough vs hand-rolling a
toolkit. Animation: CSS in GTK4; custom 60fps GLib loop for CLASS D. Particles:
Cairo offscreen, ≤200 `{x,y,vx,vy,opacity,color}`, 16ms GLib tick, composited by
wlroots. BG pipeline layers 0 nebula/mesh(Cairo) →1 particles(Cairo) →2 horizon
SVG →3 bloom(Cairo radial), all blur-passed. Fonts: Pango+Cairo+FreeType2,
bundled `/usr/share/pradyos/fonts/`, `/etc/pradyos/fonts.conf`. `ambianced`:
Rust daemon at Layer 5 init, reads kernel clock, emits ambiance to NIA bus every
60s (or on override); shell subscribes → CSS vars; cfg `/etc/pradyos/
ambiance.conf` (tz, lat/lon, boundaries).

## §12 Build order (mandatory)
7a icons (16 Sovereign Glyphs) + fonts + fontconfig → 7b wlroots skeleton
`pradyos-wm` (background + 1 window) → 7c background pipeline (4 ambiances,
static, `AMBIANCE=` env) → 7d `ambianced` + CSS var emission → 7e transition
engine (OKLab, 900s/600ms) → 7f shell panels Sovereign layout (live /proc) →
7g Manual layout + toggle spring → 7h approval popup (Sovereign Gate; stub if
L6 incomplete; ≤16ms) → 7i agent panel live (aetherd) → 7j polish (a11y,
4K 1.5×/2×, PRISM terminal, battery saver, DDRs, no TODOs).

## §13 Quality gates (per slice, before main)
WCAG AA 4.5:1 in all 4 ambiances (axe-core); no reflow on transition
(color/opacity/transform only); 60fps on VirtIO-GPU QEMU (wlroots overlay);
icons pixel-perfect 16/24/48; approval popup ≤16ms (broadcast ts → first paint);
prefers-reduced-motion kills motion w/o reflow; **no hardcoded colors in Rust/C**
— all via ambiance token system (CSS vars or `ThemeTokens` struct); every slice
has a DDR (no DDR = no merge); `clippy deny(warnings)`, zero suppression attrs.

## §14 ASI connection points (build as live consumers now)
Agent chips ← NIA `EVT_AGENT_STATE_CHANGE{agent_id,state,task_desc}` (idle/
active/thinking; tap → detail card: last 5 tasks, confidence, model). Sovereign
toggle ← reads `sovereign_gate.mode`, writes `sys_sovereign_gate_set()` (the toggle
IS the ASI on/off switch). Approval queue ← `EVT_APPROVAL_REQUIRED{action_id,
risk,agent,desc,deadline}`; APPROVE/REJECT → `sys_approval_post()`. LUMYN
self-research → pulsing indicator on its chip (tap to see exploration). Network
card → cloud calls color-coded (local Ollama = accent, cloud = bright white).
