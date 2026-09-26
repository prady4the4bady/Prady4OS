# DDR-1147 — Desktop UI/UX rebuild: Sovereign/Manual dual mode, four-ambiance theme engine (scoping)

**Status: SCOPING. Design before code (NON-NEGOTIABLE 5).** Source: PR #17
comment 5839632522 (OWNER-verified), with reference images in comment
5839590792. This is a Group E track, **independent of** DDR-1143..1146, and it
opens its own DDR series. This DDR scopes it and names the blockers. Per-piece
design DDRs follow once the blocker in §1 is cleared.

## §1 Blocker: the reference images cannot be read from this environment

The instruction makes the images the ground truth ("sample them from the actual
reference … do not guess at colors").

- Measured: every `https://github.com/user-attachments/assets/<id>` URL in
  comment 5839590792 returns **403** here, with body *"This GitHub API path is
  not available: sessions are bound to their configured repositories. Use
  repository-scoped endpoints."*
- Attachment URLs are not repository-scoped, so this session cannot fetch
  them. Routing around the proxy is not permitted, and I have not tried.
- **So nothing in this DDR comes from the images.** No palette, spacing, blur
  radius or icon shape is claimed, and none will be invented.

**Remedy (operator action):** commit the four PNGs into the repository, for
example `docs/ui/reference/{1..4}.png`. They are then readable through the
repository and git, colours can be sampled programmatically (per-panel region
medians, printed into the DDR with coordinates), and they become versioned
with the design they specify.

### §1.1 Update 2026-09-26: U0 cleared for the mockups, not for the emblem

The operator committed the four PNGs (`4ba23f0`) under their original export
names. Each carries one of the README's four panel pairings, but **not in the
order they were posted** in 5839590792, so they were matched by content, not by
position, and renamed to the README's numbers:

| File | Was | SHA-256 (first 16) | Top panel (evidence) | Bottom panel (evidence) |
|---|---|---|---|---|
| `1.png` | `…02_47_54 PM.png` | `753e028e31d0aa51` | Sovereign **Dawn**: teal, "Good morning" | Manual **Dusk**: orange sunset |
| `2.png` | `…02_50_44 PM.png` | `f1f4c12fdb391f8d` | Sovereign **Dusk**: orange, "Good evening" | Manual **Dawn**: teal |
| `3.png` | `…02_32_37 PM.png` | `70d743ee8736a210` | Sovereign **Day**: white/blue, "Good morning" | Manual **Night**: dark purple |
| `4.png` | `…02_28_47 PM.png` | `785959cdc726f45d` | Sovereign **Night**: dark purple, "Good evening" | Manual **Day**: bright lake |

Each image matches exactly one row, so the matching is unambiguous. One
inconsistency is inside the images and is recorded, not resolved: `4.png`'s
Night panel greets "Good evening" and `2.png`'s Dusk panel does too, so the
greeting text alone does not identify the ambiance; the palette does.

All four are 1536×1024, 8-bit RGB, non-interlaced, so a stdlib-only decoder
(`zlib` + PNG filters) can sample them. No Pillow dependency is needed in CI.

**Still missing:** the scorpion emblem and wordmark as vector or alpha art.
The mockups contain rendered emblems, but tracing one from a screenshot would be
a guess (§6). Icon and emblem work waits on that; palette work does not.

The images' own instruction text names Qt, Tauri, Hyprland and Wayland. The
operator's instruction overrides that: native C compositor only.

### §1.2 U-a, first cut: the accent palette, measured (2026-09-26)

**Measured with `tools/ui/png_palette.py`** (stdlib zlib plus the five PNG row
filters; no Pillow). Two findings shape the palette:

- **The ambiance is independent of the mode.** `1.png` pairs Sovereign with
  Dawn and Manual with Dusk. `2.png` pairs Sovereign with **Dusk** and Manual
  with **Dawn**. `3.png` and `4.png` do the same for Day and Night. So every
  ambiance appears once in each mode, and the palette is keyed by
  **ambiance**, not by mode.
- **The four images do not share a layout.** Anchors are chosen per image, by
  reading the image, and are listed so they can be re-measured.

`accent` mode averages the most chromatic 5% of a box. On glossy gradients a
single point is noise: point samples of the same buttons ranged from `#1B6569`
to `#BBDFDA`.

| Ambiance | Sovereign-half accents (dock active, ask button) | Manual-half accents (dock active, mode pill) | Scene mean, Sovereign half |
|---|---|---|---|
| Dawn | `#25A4AD`, `#5BABA6` (`1.png`) | `#20A2A0`, `#5FD4D3` (`2.png`) | `#648D8B` |
| Dusk | `#C45B1C`, `#CC5B1D` (`2.png`) | `#B66829`, `#A56A3A` (`1.png`) | `#774336` |
| Day | `#4E8AFC`, `#5F63F7` (`3.png`) | `#3074E9`, `#236FCA` (`4.png`) | `#DDE1EB` |
| Night | `#7C46D6`, `#723EEA` (`4.png`) | `#7355B8`, `#4C3588` (`3.png`) | `#211A38` |

Anchor boxes, `x0,y0,x1,y1`, in image order:

- **Dawn:** `1.png` 500,425,550,470 / 895,230,935,265; `2.png`
  490,870,535,910 / 25,748,215,780.
- **Dusk:** `2.png` 520,368,570,410 / 885,200,920,235; `1.png`
  465,840,512,880 / 25,710,210,745.
- **Day:** `3.png` 497,425,542,468 / 905,228,940,262; `4.png`
  420,852,465,895 / 125,828,245,862.
- **Night:** `4.png` 522,390,565,433 / 910,200,942,233; `3.png`
  467,840,512,880 / 25,710,210,745.
- **Scene boxes:** `1.png` 250,60,1130,400; `2.png` 250,60,1150,360; `3.png`
  250,60,1130,400; `4.png` 280,40,1030,380.

**Where this stops:**

- These are **measurements of the mockups, not yet palette constants.** The
  mapping to the compositor's existing OKLab 4-ambiance engine (DDR-1012) is
  U-b.
- Each ambiance's two accents per half differ by a few units of hue and
  lightness, and U-b chooses one per role.
- **Manual-half accents run darker than Sovereign-half ones in every
  ambiance.** That is consistent with the mockups' own instruction text (*"Manual
  Mode: dark, immersive"*), and it is recorded as an observation, not a rule.

### §1.3 Re-sample after the operator's corrected mapping, and a conflict it does not survive (2026-09-26)

PR #17 comment 5841547160 (**OWNER**, verified at the API: `author_association`
`OWNER`, edited 00:39:00Z) renames the modes and ambiances (Sovereign → **Regalia**,
Manual → **Consort**, Dawn → **Aurora**, Day → **Zenith**, Dusk → **Twilight**,
Night → **Umbra**), asks for the original filenames back, and gives a
"corrected" panel table. **The filenames are restored in this commit** (the
`1..4.png` renames in `0a93ece` are reverted with `git mv`; `R100`, byte-identical).

**Every panel was re-measured** (table below), and **the §1.2 values reproduce to within a
few units**, so the sampling was never the issue. What is at issue is the
**label** each panel carries, and on three of the four files the corrected
table disagrees with what the image itself prints:

| File | Operator's table: top / bottom | What the image prints: top / bottom | Evidence in the pixels |
|---|---|---|---|
| `02_28_47` | Consort Umbra / Consort Zenith | **Regalia** Umbra / Consort Zenith | Top panel's heading reads **"SOVEREIGN MODE"**, subtitle *"The machine governs. You approve."*, and the toggle sits on **SOVEREIGN**. |
| `02_32_37` | Regalia Zenith / Consort Umbra | Regalia Zenith / Consort Umbra | **Agrees.** |
| `02_47_54` | Regalia **Twilight** / Consort **Aurora** | Regalia **Aurora** / Consort **Twilight** | Top greets *"Good **morning**"* in teal. The bottom is orange. |
| `02_50_44` | Regalia **Aurora** / Consort **Twilight** | Regalia **Twilight** / Consort **Aurora** | Top greets *"Good **evening**"* in orange. This file's own instruction text says *"Sovereign Mode: … orange highlights (**dusk**) … Manual Mode: … teal highlights (**dawn**)"*. |

Read by the images, the set is **complete**: each of the 8 (mode × ambiance)
combinations appears **exactly once**, including **Regalia Umbra** (`02_28_47`
top). Read by the corrected table, Regalia Umbra has no reference and
Consort Umbra appears twice. **So the extrapolation the operator asked for may
not be needed.** Whether it is depends on which labelling is right, and that is
the operator's call. The palettes are recorded **against the file and half**,
which neither reading disputes, so nothing needs re-measuring once it is decided.

**No palette is bound to a theme name until the operator answers.** The
engine work (U-b) is held. The operator's own instruction was to report the
sampled data before going further.

Measured values (`tools/ui/png_palette.py`; `accent` = most chromatic 5% of the
box, `sample` = mean of a (2r+1)² box; coordinates in 1536×1024 image pixels):

| File · half | Dock active (accent) | Second accent (accent) | Headline "Sovereign." (accent) | Scene (sample) | Glass card (sample) |
|---|---|---|---|---|---|
| `02_28_47` top | `#7642D0` @525,392,562,432 | `#723DEB` ask btn @912,205,940,230 | `#476DE8` @660,105,825,140 | `#02030C` @700,40 r40 | `#13132A` @1300,230 r12 |
| `02_28_47` bottom | `#3073EA` @422,855,462,895 | `#226EC9` toggle @128,830,240,860 | n/a | `#939CAF` @1100,470 r30 | `#45556D` @1350,670 r10 |
| `02_32_37` top | `#4C89FC` @500,425,540,465 | `#5F63F7` ask btn @905,228,940,262 | `#2872EE` @615,130,800,170 | `#E8E9F0` @1050,120 r40 | `#E0E7F9` @1320,240 r10 |
| `02_32_37` bottom | `#7356B9` @470,840,512,882 | `#4B3486` mode pill @28,712,210,745 | n/a | `#040818` @1300,860 r20 | `#1C203B` @1300,700 r10 |
| `02_47_54` top | `#24A6AF` @505,428,545,466 | `#5AABA6` ask btn @900,230,935,265 | white text: noise | `#87A5A0` @1000,120 r40 | `#386A71` @1320,240 r10 |
| `02_47_54` bottom | `#B46526` @468,842,510,882 | `#A56A3A` mode pill @25,710,210,745 | n/a | `#1B181C` @1300,860 r20 | `#462E27` @1300,700 r10 |
| `02_50_44` top | `#C95B1A` @527,372,565,408 | `#CF5C1C` ask btn @888,203,918,233 | white text: noise | `#643733` @900,100 r40 | `#4A2E2B` @1330,210 r10 |
| `02_50_44` bottom | `#20A3A1` @493,870,533,910 | `#5FD4D3` mode pill @25,748,215,780 | n/a | `#56858A` @1100,580 r30 | `#214045` @1350,745 r10 |

The headline column is meaningful only where the word is coloured
(`02_28_47`, `02_32_37`). Where it is white, the most chromatic pixels in
the box are background bleeding through the glyph edges, so it is marked
noise rather than reported as a colour.

**Emblem / wordmark (step 2 of the instruction): still blocked, named.** No
emblem artwork is in the tree, only raster screenshots. The operator's own
instruction forbids tracing it and calling it final. The asset needed is an
**SVG** of the scorpion emblem and the PRADYOS wordmark, or a **PNG with an
alpha channel at ≥1024 px**, one colour on transparent.

### §1.4 Operator decision 1: mechanical classification by a fixed colour rule (2026-09-26)

PR #17 comment 5845610518 (**OWNER**, verified at the API) settles §1.3's conflict
by **neither** labelling. Theme names are assigned by a fixed colour convention,
*"cool/blue-teal → Aurora (Dawn); bright/light → Zenith (Day); warm
orange/amber → Twilight (Dusk); dark → Umbra (Night)"*. Each panel is classified
mechanically by `tools/ui/png_palette.py` (*"do not eyeball it"*), and this
section records every cell that changes against **both** earlier readings.

**The rule, as code (`png_palette.py classify`).** Each half is measured over
all its pixels: mean OKLab lightness `L`, the hue of its most chromatic 5%, and
`warm` = the share of chroma at hue 20–100° against cool hue 150–300°. Then, in
order:

1. `L ≥ 0.70` → **Zenith**
2. `warm ≥ 0.50` → **Twilight**
3. `L ≤ 0.32` → **Umbra**
4. otherwise → **Aurora**

Brightness is tested first because a bright panel can also be blue.
Each threshold sits in the largest gap of the measured distribution. The halves
split at row 512, and the result is identical at rows 480 and 535.

| File · half | L | hue | warm | Rule gives | Captions said | Operator's table said |
|---|---|---|---|---|---|---|
| `02_28_47` top | 0.272 | 298 | 0.00 | **Umbra** | Umbra | Umbra |
| `02_28_47` bottom | 0.426 | 263 | 0.01 | **Aurora** | Zenith | Zenith |
| `02_32_37` top | 0.825 | 262 | 0.00 | **Zenith** | Zenith | Zenith |
| `02_32_37` bottom | 0.217 | 288 | 0.00 | **Umbra** | Umbra | Umbra |
| `02_47_54` top | 0.469 | 205 | 0.03 | **Aurora** | Aurora | Twilight |
| `02_47_54` bottom | 0.280 | 47 | 0.74 | **Twilight** | Twilight | Aurora |
| `02_50_44` top | 0.327 | 45 | 0.80 | **Twilight** | Twilight | Aurora |
| `02_50_44` bottom | 0.364 | 202 | 0.01 | **Aurora** | Aurora | Twilight |

Margins: the Zenith call clears its threshold by 0.125. The nearest Umbra call
(`02_28_47` top, 0.272) clears by 0.048, and the nearest Aurora-not-Umbra call
(`02_50_44` bottom, 0.364) by 0.044. The warm split is 0.74/0.80 against ≤ 0.03.

**Reassignments.** Against the **captions**, one cell changes: `02_28_47` bottom,
Zenith → Aurora. Against the **operator's table**, five change: that same cell,
plus both halves of `02_47_54` and both halves of `02_50_44` (each pair swapped).
Mode per half is not in dispute. Every top half is Regalia, both by caption and
by the "SOVEREIGN MODE" heading printed in three of the four.

**The rule leaves the set with a duplicate and a hole.** Consort gets Aurora
**twice** (`02_28_47` bottom and `02_50_44` bottom) and no Zenith. Regalia is
complete. The engine needs all eight cells, so a **second mechanical step** is
applied, stated here and implemented in the same script:

- **A duplicated cell keeps the candidate whose dock accent is nearest (OKLab
  distance) to the other mode's accent for the same ambiance.** Consort Aurora
  against Regalia Aurora (`#24A6AF`): `02_50_44` bottom (`#20A3A1`) = **0.021**,
  `02_28_47` bottom (`#3073EA`) = 0.185. `02_50_44` bottom keeps the cell.
- **A displaced candidate fills that mode's empty cell** whose other-mode accent
  is nearest to it. `02_28_47` bottom against the Regalia accents: Zenith 0.068,
  Umbra 0.132, Aurora 0.185, Twilight 0.335. It fills **Consort Zenith**.

**Stated plainly, the second step overrides the first on exactly one cell:**
`02_28_47` bottom is classified Aurora by the colour rule and bound to Zenith by
the resolution rule. Two facts support that. Its mean lightness is low
(0.426, a dim scene of a bright lake), while its **accent** is the Zenith blue
darkened, as every Consort accent is relative to its Regalia counterpart
(§1.2). If the operator prefers a different resolution, only the assignment
list in the script changes. The measurements do not.

### §1.5 U-b/U-c/U-d as built from decisions 2 and 3 (design, written before the code)

Decision 2: proceed with palette extraction, the 4-ambiance theme engine, and the
Regalia/Consort toggle. The emblem stays deferred as its own checklist line.
Decision 3: build natively in `user/compositor.c` on PradyOS's own syscalls, and
ignore the Qt/QML/React/Tauri/Hyprland/Wayland notes printed in the images.

**Palette provenance: generated, not transcribed.** `png_palette.py header`
decodes the four committed PNGs, runs §1.4's classification and resolution, and
samples each cell's dock-active accent (the §1.3 anchor boxes). It writes
`user/theme_palette.h`, which is marked *do not edit*. `make ci-palette-check`
regenerates the header into `build/` and `cmp`s it against the committed copy.
It is wired into `hygiene_check.sh`, making the tenth static check. A hand edit
to a value, a changed threshold, or a changed resolution all fail it. This is
DDR-1053's `fetch_mldsa_kat.py` shape: a committed tool is the provenance, not a
session artefact.

**What the engine binds, and what it deliberately does not.**
- **Bound:** `THEME_AC[mode][ambiance]`, the accent, which is what `g_ac`
  already paints (dock highlight, focused title bar, accent bar, Manual
  taskbar). `set_ambiance()` now targets `THEME_AC[SYS_GET_MODE][idx]` instead
  of the mode-blind `AMB[idx].ac`. A mode change re-targets the accent with a
  2-frame transition inside `render_and_announce`: one extra full-screen render
  (~0.93 s under TCG, DDR-1029).
- **Not bound: the backdrop base `bg`.** The measured scene colours are
  **averages of photographs** (§1.3's `sample` column), not flat bases. Using
  them would paint Zenith's `#E8E9F0` behind white text that the compositor
  draws at fixed colours. The existing dark bases stay until the per-widget text
  colours become theme-driven (U-e/U-g). The glass-card colours are recorded and
  not bound, for the same reason.
- **The final transition frame is exact.** `lab_lerp` at `t = 1` round-trips
  through float OKLab and can land ±1 from the target. The settled frame now
  copies the target bytes, so a readback can assert exact values.

**Names.** The gate-asserted identifiers `DAWN/DAY/DUSK/NIGHT` (four Makefile
gates) and `SOVEREIGN/MANUAL` stay unchanged as wire tokens. The operator's
names are **display names**: `Aurora/Zenith/Twilight/Umbra` and
`Regalia/Consort`. They go in a new `PRADYOS_THEME` line and in the on-screen
mode titles (`"REGALIA"`/`"CONSORT"` replace `"SOVEREIGN MODE"`/`"MANUAL
MODE"`, measured to be asserted by no gate).

**U1, the toggle with a confirm step and an audit record.**
- **The audit half was missing, and it is a kernel change.** `sys_set_mode`
  audits a **refusal** (`AR_CAP_DENIED`) but a **successful** mode change
  writes nothing, which was measured in `sys_aether.c` and `aether_queue.c`.
  A change that alters agent approval policy leaves no record. `AR_MODE_SET` is
  **appended** to the audit enum (value 28, pinned with `_Static_assert` per
  DDR-832). It is recorded on every successful call, with `action_id` =
  `(previous << 32) | requested`, so privacy on/off (modes 2/3) is recorded too.
- **The confirm half is in the compositor.** Super+M no longer switches. It
  **arms** a pending switch and prints `PRADYOS_MODE_CONFIRM pending to=N`.
  **Enter** commits: `SYS_SET_MODE`, the existing
  `PRADYOS_SUPERKEY_TOGGLE from= to=` line, then a **read-back of the kernel's
  own audit record** via `SYS_READ_AUDIT`, printed as
  `PRADYOS_MODE_AUDIT found=1 prev= new=`. **Esc** cancels, and so does any
  other key. A pending switch **expires after 10 s** of wall time
  (`SYS_CLOCK`), so a stray Enter much later cannot flip a security-relevant
  setting.
- **Gate (`smoke-superkey`), with the key list rewritten** to
  `m meta_l-m ret meta_l-m esc meta_l-m ret ctrl-c`. The arms are: the
  **exact ordered sequence** of the first round's six mode lines (pending/
  toggle/pending/cancel/pending/toggle), and `found=1` on both commits. The
  ordered sequence is what catches the obvious mutant: a no-confirm compositor
  still prints both `from=0 to=1` and `from=1 to=0`, so the old two arms pass
  it. `found=1` catches a kernel that stops auditing, since the compositor
  cannot manufacture that record.
- **Palette arm (`smoke-ambiance`).** The boot demo cycle prints
  `PRADYOS_THEME mode=<0|1> amb=<TOKEN> name=<display> ac=<RRGGBB>` from the
  **settled `g_ac`**. The gate asserts the four exact accents for the mode that
  prints, taking the expected values from `user/theme_palette.h`, not from a
  second hand copy. A mode-blind engine (the pre-change behaviour) prints
  `AMB[]`'s old accents and fails.

**Residual, recorded rather than claimed:** the bare single-key `s`/`m` hooks
(and `p` poweroff, `b` reboot, `q` exit) remain **unconfirmed debug hooks**, and
three gates depend on them (`smoke-compositor`, `smoke-motion`,
`smoke-superkey`'s reset). U1's confirm covers the user-facing chord. The
kernel audit covers **every** path, hooks included, because it sits below all
of them.

## §2 What the compositor already has (measured in `user/compositor.c`, 1,992 lines)

| Capability | State | Where |
|---|---|---|
| Per-pixel alpha blending | **Exists** (float alpha over the framebuffer) | `blend_px` |
| Backdrop blur | **Exists, limited**: separable box blur, radius 4, plus 1.3× saturation (DDR-722) | `blur_rect`, `glass_card` |
| Four ambiances (DAWN/DAY/DUSK/NIGHT) with OKLab interpolation | **Exists** (DDR-709/716/1012), with its own placeholder palette | `AMB[4]`, `rgb2lab`/`lab2rgb` |
| Backdrops: horizon bands, particles, glows | **Exists** | DDR-1012, DDR-716 |
| Anti-aliased text | **Exists** (Inter atlas, `user/inter_font.h`), alongside an 8×8 bitmap font | `term.c`, `compositor.c` |
| Dock, windows, drag/resize, Alt-Tab, per-window restore | **Exists** (Group E, all gated) | DDR-715..1008 |
| Agent roster panel with the eight names | **Exists as labels**; slots are generic (DDR-1022: one agent program) | compositor + `SYS_AGENT_ROSTER` |
| Display paths | virtio-gpu and GOP (DDR-1142, `smoke-gop`) | |

**So "glassmorphism" is not missing a rendering primitive. What it lacks is
fidelity and cost.**
- **Fidelity:** the blur is a radius-4 box, not a Gaussian. Whether that
  matches the reference is a question the images answer (§1). A larger radius
  costs O(radius) per pixel per pass unless it moves to a running-sum box
  (O(1) per pixel). Three box passes approximate a Gaussian. That is an
  engine change, named here and not faked.
- **Cost:** DDR-1029 measured one full-screen render + present at **~0.93 s
  under TCG**. The 300–500 ms mode transition and the spring magnify-on-hover
  **cannot be observed as animations under TCG**. They can be implemented
  (time-based interpolation against `SYS_CLOCK`/ticks, not frame counts, so
  they degrade to fewer frames rather than slower motion). Only their
  **endpoints and timing arithmetic** are gateable here. **Perceived
  smoothness needs real hardware.** No FPS figure will be claimed.
- **Damage tracking:** the compositor repaints whole frames. A sidebar that
  updates live (rings, network graph) at full-frame cost under TCG would
  starve input (DDR-1028/1029). **Dirty-rectangle repaint is a real engine
  prerequisite** for the live widgets, and it is named as its own piece.

## §3 Real data vs placeholder, stated per widget

| Widget | Real source today | Verdict |
|---|---|---|
| RAM ring | `SYS_MEMINFO` (total/free/used pages) | **Real** |
| CPU ring | `SYS_GETPROCS` run_ticks deltas plus `SYS_SYSINFO` uptime/cpu_count. Utilisation = non-idle ticks / elapsed, per window. | **Real, derived.** The derivation is gated against a known busy-loop probe. |
| GPU ring | Nothing. virtio-gpu here is 2D scanout with no utilisation concept, and GOP has none either. | **Placeholder, labelled "n/a"**, never a fake number |
| Disk ring | Capacity only exists once DDR-1143's persistent root exists. No I/O counters are exported. | **Placeholder until DDR-1143**; then capacity is real, and I/O rate needs a new counter syscall |
| Network graph | lwIP runs with `LWIP_STATS 1` internally, but **no syscall exports it**. DHCP/DNS state (DDR-1141) exists. | **Needs a new read-only syscall** (rx/tx bytes and packets, plus lease state). Small, and real once added. |
| AI Agents panel, 8 names | Roster: KRYOS and SOLIN appear in kernel roster code. PRAX, LUMYN, AHNIS, IRIS, RUFLO and HERMES exist only as compositor labels. AHNIS/IRIS map to CAP_OCR/CAP_SCENE, which are **deferred (DDR-982 §5.3)**; PRAX to CAP_EXEC/ACTION_EXEC_CODE (a **pre-approved exception**); LUMYN to CAP_NET_BROWSE (deferred). | **Real:** slot occupancy, pid and dispatches from `SYS_AGENT_METRICS` for whatever occupies a slot. **Placeholder:** any named agent whose capability is deferred, shown as "deferred", not "idle" |
| Quick settings: WiFi | No WiFi driver (virtio-net/e1000e only) | **Placeholder**; the toggle can map to the network interface up/down only if that is wanted |
| Bluetooth | No stack | **Placeholder** |
| Volume | No audio (Intel HDA is a pre-approved exception) | **Placeholder** |
| Brightness | No backlight control (no ACPI `_BCM`, and VMs have none) | **Placeholder** |
| CPU/GPU/RAM/Disk "sliders" | These read as controls. No resource-limit knobs exist except ADR-032's FS write bucket. | **Read-only meters.** Real ones are real per the rows above. They are not controls unless the operator names what they should control. |
| Search bar, "Ask PRADYOS anything" | No in-tree model. The only inference path is off-box Ollama (DDR-1110, F#73 relocated blocker). | **UI plus routing to the AETHER daemon's existing test-mode path**; not a claim of local NL capability |
| Files quick-access, recent files | VFS, `SYS_GETDENTS`. Directories are indistinguishable from files at the syscall boundary (DDR-1101 §b). | **Partially real.** A folder tree needs DDR-1101's `S_IFDIR` work first. |
| PRISM terminal panel | `user/term.c` (DDR-1027) | **Real** |

## §4 Theme persistence and auto mode

- **Automatic:** the RTC via `SYS_TIME` (DDR-749). Boundaries (dawn/day/dusk/
  night) are fixed clock hours unless the operator wants solar times, which
  need a location. No location source exists, so that is named, not built.
- **NTP: not available.** DDR-1141 built DHCP and DNS only; there is no SNTP
  client. It is a separate follow-on (small: one UDP exchange, but it needs
  the ring-3 UDP door, Group C, and DDR-1091's per-send allowlist question).
- **Persistence:** "pinned theme vs automatic" must survive reboot. On the
  **live** ISO the root is a RAM disk, so persistence only exists after
  DDR-1143. Before that, the setting is session-only, and the UI says so.
  Mechanism: a small settings file on the persistent root, read by the
  compositor at start, written atomically (write-new + rename, DDR-956).
- **Reversible toggle:** Settings → Theme: Auto | Dawn | Day | Dusk | Night.
  Selecting Auto clears the pin. Gate arm: pin → reboot → still pinned; auto
  → reboot → follows RTC. The reboot arm needs DDR-1143.

## §5 Mode toggle semantics

- **Sovereign vs Manual already has a kernel meaning:** `SYS_SET_MODE` and
  AETHER's sovereign mode (auto-approve vs force-pending, DDR-842 S4).
- The UI toggle must decide whether it **is** that switch or only a view.
  Making it the real switch means a click changes agent approval policy.
  That is a security-relevant control, so it needs confirmation and an audit
  record.
- **Decision U1 for the operator.** Recommendation: the toggle drives the real
  mode, with a confirm step and an `aether_audit` record. A view-only toggle
  labelled "Sovereign" that changes nothing would be the DDR-1059 shape.

## §6 Icons and wordmark

- Original icon set, drawn as vector paths rasterised into the atlas at build
  time, or hand-authored alpha masks.
- **The scorpion emblem and wordmark:** the only existing branding is the
  provisional placeholder (checklist #14, "provisional branding" disclaimer).
  Recolouring "as shown" needs the emblem artwork itself, which is in the
  images I cannot read (§1). The operator should supply it as a vector (SVG)
  or a high-resolution alpha mask in the repository. Tracing it from a
  screenshot would be a guess.

## §7 Proposed piece breakdown and size (estimates, not measurements)

| # | Piece | Approx. new code | Risk | Gateable here |
|---|---|---|---|---|
| U-a | Palette extraction from the committed references into a generated header, with a host script | 150 host + 100 C | Low | Yes, byte-exact against the script's output |
| U-b | Engine: dirty-rect repaint; running-sum 3-pass box blur (Gaussian approx.); time-based animation clock | 600–900 | **High**: touches every frame of the most-gated user program (17 Makefile gate lines match compositor/window/dock/ambiance names, measured by grep) | Endpoints and blur kernel exactness via readback; smoothness **no** |
| U-c | Theme engine: four palettes from U-a, auto (RTC), pin, persistence | 300–450 | Medium; persistence depends on DDR-1143 | Yes (readback of sampled pixels, as DDR-1012 does) |
| U-d | Sovereign/Manual toggle + transition + tagline (U1) | 250–400 | Medium (security semantics) | Yes: mode read back from the kernel plus audit record |
| U-e | Sovereign chrome: greeting, search bar, quick-launch row, dock with magnify | 700–1,000 | Medium | Layout via `PRADYOS_WM_GEOM`-style geometry lines; spring timing arithmetic only |
| U-f | Right sidebar: rings, network graph, agents panel | 500–700 + ~80 kernel (net stats syscall, next free NSI) | Medium | Real values: a gate forces a known load and asserts the ring moves; placeholders asserted as "n/a" |
| U-g | Manual desktop: file browser, terminal panel, quick settings | 800–1,200 | Medium; the file tree is limited by DDR-1101 | Yes, for the real parts |
| U-h | Icon set + recolourable emblem (needs artwork, §6) | 300–600, plus assets | Low in code; the assets are external | Yes (atlas hash) |
| | **Total** | **~3,900–5,500 lines** | | |

Roughly **6–9 working sessions**, excluding waiting on §1 and §6.

**Kernel image:** the compositor is embedded, so its growth lands in
`kernel.bin`, with 220,790 B of headroom. Icon atlases and a larger Inter
atlas could consume a large share. If assets are large, the compositor or its
assets move to a non-embedded ELF/file, as `term.elf` is. That depends on a
persistent root (DDR-1143) or the FAT boot volume, so it is flagged now.

## §8 What needs real hardware or a person to verify

- **Animation smoothness**, transition feel and spring physics feel.
  TCG renders about one frame per second (DDR-1029).
- **Colour accuracy on a real panel.** Pixel readback proves the framebuffer
  values, not what a display shows.
- **Visual fidelity against the references.** The gates can assert sampled
  pixels and geometry. Whether it "looks like the image" in full is a human
  review. I will produce screendumps (QMP, as `smoke-gop` does) for the
  operator to compare side by side, and will not call that automated.

## §9 Decisions needed

| # | Decision | Recommendation |
|---|---|---|
| U0 | Commit the four reference PNGs (and the emblem as vector art) into the repository | Required before any palette or icon work (§1, §6) |
| U1 | Does the Sovereign/Manual toggle drive the real AETHER mode? | Yes, with a confirm step and an audit record |
| U2 | Auto-theme boundaries: fixed clock hours, or solar times from a configured location | Fixed hours for v1 |
| U3 | Quick-settings "sliders": read-only meters, or what should they control? | Read-only meters |
| U4 | Placeholders (GPU, WiFi, Bluetooth, volume, brightness): show as disabled/"n/a", or hide | Show as disabled with "not available on this build" |

### §9.1 Decided 2026-09-26 (PR #17 comment 5841525203, OWNER)

- **U0:** mockups committed (§1.1). Emblem/wordmark art still pending.
- **U1:** the toggle drives the real AETHER mode (`SYS_SET_MODE`), with a
  confirm step and an audit record. Not cosmetic.
- **U2:** fixed clock hours for v1.
- **U3:** read-only meters.
- **U4:** unavailable hardware shown disabled, labelled "not available on this
  build".

## §10 Not claimed

- Nothing above §1.1 is derived from the reference images. §1.1 records only the file-to-panel mapping; palette values come in piece U-a, sampled by script.
- No animation smoothness or frame rate is claimed.
- No local natural-language capability is claimed for the search bar.
- No mobile UI; that is explicitly out of scope.
- No implementation exists.
