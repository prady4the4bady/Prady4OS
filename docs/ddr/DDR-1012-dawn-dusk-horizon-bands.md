# DDR-1012 — DAWN/DUSK horizon bands (Group E, the last DDR-716 non-goal that is buildable)

**Status:** DESIGN. Implements the Group E row "OKLab horizon bands / animated
mesh (DDR-716 deferred)" — the **bands**. The animation is assessed and
deliberately not built; see §5.

---

## 1. What DDR-716 deferred, and what is worth building now

DDR-716's non-goals: *"Mesh/nebula animation (20 s / 120 s drifts); the DAWN/DUSK
horizon bands; real Gaussian blur + saturation; multi-stop linear gradients;
per-star glow bloom."*

Two of those five have since been built by other work — DDR-722 shipped a real
separable box blur with saturation boost (`blur_rect`), and DDR-723 shipped the
multi-stop vertical gradient. **The horizon bands are the remaining piece that is
both unbuilt and cheap.**

They also fill a genuine hole: `render_backdrop`'s DAWN arm is literally

```c
case 0:                                        /* DAWN: motes carry it */
    break;
```

DAWN is the only ambiance with **no backdrop at all**. And DUSK has a sun-bloom
centred at (85%, 90%) with nothing for it to sit on — a sun with no horizon.

## 2. The primitive

`horizon_band(cy, half, r, g, b, peak_a)` — a full-width band centred on row
`cy`, alpha falling off quadratically from `peak_a` at the centre to 0 at
`cy ± half`. It reuses DDR-712's `blend_px`, exactly as `radial_glow` does.

Cost is `2 * half * width` blends. At `half = 44` on a 1024-wide screen that is
~90k — **cheaper than a single radial**, whose 300 px disc is ~280k. So this adds
less per-frame work than any backdrop element already shipped, and it inherits
DDR-716 D3's guard: backdrops draw only on **settled** frames, never during the
6–8 intermediate frames of an OKLab transition.

## 3. Placement

| ambiance | band centre | half-height | colour | peak α |
|---|---|---|---|---|
| DAWN | 62% of height | 44 px | `0xFF,0xA8,0xC0` (rose) | 0.18 |
| DUSK | 88% of height | 40 px | `0xFF,0x9A,0x3C` (amber) | 0.22 |

DUSK's band sits at 88% and the sun-bloom is centred at 90%, so the bloom rises
*out of* the band rather than floating above it. DAWN's sits higher and cooler,
which is the point of the pair — DDR-716's own framing is that the four ambiances
have *signature* backgrounds, and DAWN currently has none.

DAY and NIGHT get no band: DAY is a three-node mesh (no horizon in that language)
and NIGHT's two nebulas are deliberately unanchored.

## 4. The gate, and why a sentinel alone would be vacuous

`smoke-horizon` (shard 9, `fast`). The obvious gate — grep for a
`PRADYOS_HORIZON` line — tests a `printf`, not a renderer. The same trap
DDR-973 §6 caught with a chain-repeat mutant and DDR-1008 caught with restore-all:
**an implementation that prints the sentinel and draws nothing passes.**

So the compositor **reads its own framebuffer back** after drawing and publishes
what it measured:

```
PRADYOS_HORIZON DAWN y=476 in=<pixel at band centre> out=<pixel 8px above the band>
PRADYOS_HORIZON_OK
```

### 4.1 The first version of this assertion was vacuous, and was replaced before it ran

The design above originally compared the band centre against a row *above* the
band (`in` vs `out`). **That tests nothing.** `render()` lays a per-row vertical
gradient (DDR-723), so two different rows differ whether or not a band was drawn
— the assertion would have passed on a no-op band, which is precisely the failure
it exists to catch.

The comparison that isolates *this draw* is the **same pixel, before and after**.
`horizon_band` samples `(w/2, cy)` on entry and again on exit, and the compositor
publishes both:

```
PRADYOS_HORIZON DAWN y=476 pre=18092C post=412546
```

The exit sample is taken **inside** `horizon_band`, not by the caller, because
DUSK lays its sun-bloom down immediately afterwards and would contaminate a
later reading.

Caught by reading the assertion against `render()`'s gradient, before the gate
was ever executed.

## 5. The animation is assessed and NOT built

DDR-716 also deferred the 20 s mesh drift and the 120 s nebula drift. Not built,
and this is a decision rather than an omission:

- **It cannot be gated meaningfully in the time available.** A 120 s drift does
  not fit in a 120 s gate window, and a gate that asserts "two frames 20 s apart
  differ" would pass on any per-frame noise.
- **It costs frame time on every settled frame, permanently.** DDR-989 found
  vruntime sampling starvation that compositor load aggravates, and DDR-1007 just
  raised the maximized-window blit by 2.2×. Adding a continuous full-screen
  recomputation days before a release, to a compositor that has twice been the
  subject of scheduling defects, is the wrong trade.
- The static backdrops already deliver DDR-716's stated goal — *"completing the
  four ambiances' signature backgrounds"*.

Logged as `[DEFERRED: animation — cannot be gated inside a 120 s window; costs
per-frame work on a compositor with two open scheduling defects]`.

## 6. What must be measured

1. `smoke-horizon` green, with `in != out` for **both** DAWN and DUSK.
2. **Mutation M1:** publish the sentinel and the samples but skip the
   `horizon_band` call. The gate must FAIL — that is the proof it tests pixels.
3. `smoke-backdrop`, `smoke-ambiance`, `smoke-gradient`, `smoke-cadence`
   unchanged: the bands draw inside `render_backdrop`, which those gates exercise.
4. Kernel hash recorded (R1), `-Werror` clean, `kernel.bin` under 1,572,864 B.


---

## 7. MEASURED

Kernel **`9623c163cd479043`**, warning-clean at `-Werror`, `kernel.bin`
**1,102,218 B** against the 1,572,864 B gate.

### 7.1 The gate measures pixels

```
[horizon] DAWN pre=18092C post=412546
[horizon] DUSK pre=290E00 post=582C0D
[horizon] PASS — DAWN and DUSK bands measured in the framebuffer
```

Both shifts are consistent with the specified blends — DAWN's rose `0xFFA8C0` at
α=0.18 lifts R `0x18→0x41`, DUSK's amber `0xFF9A3C` at α=0.22 lifts R
`0x29→0x58`. These are the compositor reporting its own framebuffer, not its
intent.

### 7.2 M1 — the band draws nothing

Mutant: keep both samples and the sentinel, delete the blend loop. Kernel
`a2dccf7ad726ed55` (distinct hash — verified, not assumed).

```
[horizon] DAWN pre=18092C post=18092C
[horizon] FAIL — DAWN centre pixel unchanged across the band draw; the band is a no-op
```

**Caught — and note what did NOT catch it.** `boot_test.sh`'s `EXTRA_SENTINEL`
check passed on the mutant: `PRADYOS_HORIZON DAWN`, `PRADYOS_HORIZON DUSK` and
`PRADYOS_HORIZON_OK` were all present. A sentinel-only gate — the shape every
other Layer-7 backdrop gate uses — would have reported PASS on a compositor that
drew nothing at all.

### 7.3 Regression

| gate | result |
|---|---|
| `smoke-backdrop` | PASS |
| `smoke-ambiance` | PASS |
| `smoke-gradient` | PASS |
| `smoke-cadence` | PASS |
| `smoke-shell` | PASS |

`ci-shard-check` OK (158 gates / 10 shards / 7 excluded);
`ci-probe-rodata-check` OK (61 ELFs).

### 7.4 What is NOT measured

- **The bands' appearance.** The gate proves the centre pixel moved by roughly
  the specified alpha toward the specified colour. It does not prove the result
  looks like a horizon; nothing automated can.
- **Frame cost.** ~90k blends per settled frame was computed from `2*half*width`,
  not timed. It is bounded by construction and smaller than a single radial, but
  no measurement was taken.
- **The animation**, deliberately — §5.
