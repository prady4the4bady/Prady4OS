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
