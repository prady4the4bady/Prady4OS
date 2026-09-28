# DDR-1007 — Window maximize at real display size

**Status:** DESIGN. Implements the Group E row "Window maximize at real display
size (DDR-719 caps at 512×512)".
**Corrects** the `NEXT_TASK_QUEUE` note attached to that row (see §8).

---

## 1. What the row asks for, and why it is not a one-line change

DDR-719 maximizes by asking the owner for **512×512** and moving the window to
(8, 26):

```c
nsi(SYS_SURFACE_SENDEV, (long)id, 1, (512L << 16) | 512L);   /* compositor.c:1310 */
nsi(SYS_SURFACE_MOVE,   (long)id, 8, 26);
```

The display is **1024×768** (`virtio_gpu.c:130`, with scanout 0's mode used when
sane). So a "maximized" window covers 26% of the screen.

512 is not an arbitrary constant. It is `SURFACE_DIM_MAX`
(`sys_surface.c:17`), and **three separate limits are pinned to it**, only one of
which is written down:

| limit | value | where | tied to 512 how |
|---|---|---|---|
| per-dimension cap | `SURFACE_DIM_MAX = 512` | `sys_surface.c:17` | stated: "per-surface buffer <= 1 MiB (512*512*4)" |
| **per-surface VA window** | `SURFACE_VA_SLOT = 0x100000` (**1 MiB**) | `sys_surface.c:19` | **unstated** — happens to equal 512×512×4 exactly |
| largest representable buddy order | `order_for` clamps at `o < 10` → 4 MiB | `sys_surface.c:65-69` | silent clamp, currently unreachable |

The second is the dangerous one. Surface `id`'s buffer is mapped at
`SURFACE_VA_BASE + id * SURFACE_VA_SLOT` and `sys_surface_map` maps
`s->npages` pages from there with **no check that npages fits the slot**:

```c
uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
for (uint32_t i = 0; i < npages; i++)
    vmm_map_in(current_thread->cr3, va + i * PAGE_SIZE, phys + i * PAGE_SIZE, …);
```

Raising `SURFACE_DIM_MAX` **alone** would therefore map surface *N*'s buffer
across surface *N+1*'s VA window. Both mappings are `SURF_VIEW_FLAGS`
(user-visible), and the compositor maps every surface into one address space via
`sys_surface_map_ro` — so surface N+1's pixels would be silently replaced by the
tail of surface N's buffer, in a process that maps windows belonging to **other
processes**. That is a cross-window data leak, not a rendering artefact.

It is exactly §INV.13's PT_HI lesson in a new place: one quantity, two
definitions, one of them undocumented.

## 2. The three limits, made explicit

```c
#define SURFACE_DIM_MAX    1024u                /* >= the 1024x768 scanout */
#define SURFACE_BYTES_MAX  (4u * 1024u * 1024u) /* one buddy order-10 block   */
#define SURFACE_VA_SLOT    0x400000ull          /* 4 MiB — MUST be >= BYTES_MAX */
```

and a compile-time tie so the next raise cannot repeat §1:

```c
_Static_assert(SURFACE_BYTES_MAX <= SURFACE_VA_SLOT, …);
_Static_assert((uint64_t)SURFACE_DIM_MAX * SURFACE_DIM_MAX * 4 <= SURFACE_BYTES_MAX, …);
```

`SURFACE_BYTES_MAX` is a **new** check, not a restatement: today `w,h <= 512`
bounds the product implicitly, and at `DIM_MAX = 1024` the worst case
(1024×1024×4) lands exactly on 4 MiB. "Exactly" is why the assertion is worth
having — the byte cap is the quantity that actually matters, and the next person
to raise `DIM_MAX` to 1280 gets a build error instead of a `order_for` clamp
that silently under-allocates by half.

VA span check: 16 slots × 4 MiB = 64 MiB. `SURFACE_VA_BASE = 0x8600000000`, so
the region ends at `0x8604000000`, still far below the framebuffer mapping at
`0x8700000000`. No overlap.

### 2.1 `order_for`'s silent clamp

```c
while (((uint64_t)1 << o) < npages && o < 10) o++;
```

If `npages > 1024` this returns 10 and the caller allocates 4 MiB for a larger
request — a heap overflow the moment it is reachable. The `_Static_assert` above
makes it unreachable **by construction** rather than by the reader noticing.
`order_for` itself is left alone: changing its signature would touch the two free
paths, which pass an already-validated `onpages`, for no gain.

## 3. Maximize target: the work area, not the raw display

"Real display size" cannot mean the whole framebuffer, because the compositor
draws chrome that a maximized window must not cover — and the chrome **differs by
mode** (DDR-893 made Manual a structurally different desktop, not a restyle):

| mode | top chrome | bottom chrome | right chrome |
|---|---|---|---|
| Sovereign | 6 px accent bar (`compositor.c:550`) | none | agent panel, 210 px (`compositor.c:422`) |
| Manual | 18 px menu bar (`MANUAL_MENUBAR_H`) | 28 px taskbar (`MANUAL_TASKBAR_H`) | none — Manual has no agent panel |

So maximize targets a `work_area()` derived from `g_fi` and the live mode, with
an 8 px margin, and the window's **content** origin sits `TITLEBAR` (18 px) below
the work-area top because the title bar is drawn at `s->y - TITLEBAR`.

At 1024×768 in Sovereign that gives content `x=8, y=32`, `w=790, h=728`
(vs. today's 512×512 at (8,26)) — **4.3× the area**.

The result is clamped to `SURFACE_DIM_MAX` in both axes, so a hypothetical
larger scanout degrades to the largest legal surface instead of failing.

## 4. The drag-resize ceiling moves with it

`compositor.c:1457-1458` clamps interactive resize to 512 as well. Leaving that
at 512 while maximize reaches 790 would mean a user can maximize to a size they
cannot then drag to — an arbitrary asymmetry. Both now derive from one helper.
The existing drag gates (`smoke-evresize`, `smoke-drag`, `smoke-resizeall`)
target sizes far below either ceiling, so raising it does not move any assertion
they make.

## 5. The gate, and the §INV.5 repair that comes with it

`smoke-wmmax` currently asserts a **constant**:

```make
@grep -q "PRADYOS_EV_RESIZE_OK w=512 h=512" build/wmmax.log || …
```

That constant is now wrong, and hardcoding the new one would be the same defect
with a bigger number — and a §INV.5 violation besides ("Geometry in gates:
`PRADYOS_WM_GEOM` fields. Never hardcoded pixel coords.").

So the compositor now publishes the size it asked for:

```
PRADYOS_WM_MAX id=1 w=790 h=728
```

and the gate **extracts w/h from that line** and asserts the client's ack
matches. This is strictly stronger than the old check: it verifies the owner
honoured *the size actually requested*, where before it verified the owner
printed a number the gate already knew. A compositor that requested 790 and a
client that acked 512 passed nothing before and fails now.

The second mouse injection is armed on `PRADYOS_EV_RESIZE_OK w=512`; it becomes
the size-agnostic prefix `PRADYOS_EV_RESIZE_OK w=`. In `smoke-wmmax` there is no
drag, so the first such line is the maximize ack.

**Already fixed, and not re-fixed here:** DDR-975 §7/§8.1 flagged that both
injections used hardcoded `ABSX/ABSY`. They no longer do — the recipe passes
`GEOM_TITLE=BETA GEOM_FIELD=mx` and `mouse_inject.sh` reads `PRADYOS_WM_GEOM`.
That repair landed between DDR-975 and now; §8 below corrects the queue note
that still points at it.

## 6. Cost, stated rather than discovered later

- **Physical memory.** A maximized surface allocates a 4 MiB buddy block for a
  ~2.2 MiB buffer (order-10 rounding). QEMU is started with no `-m`, i.e. the
  128 MiB default. One or two maximized windows is fine; sixteen is not, and
  sixteen was never possible at 512 either (16 MiB then, 64 MiB now).
- **Frame cost.** `blit_surface` is a per-pixel loop with a bounds test.
  790×728 = 575k px against 512×512 = 262k px — **2.2× per maximized window per
  frame**. `smoke-wmmax` runs in a 120 s window and must still make its second
  injection deadline; if it does not, that is a measurement to record, not a
  reason to quietly shrink the target back.

## 7. What must be measured before this is claimed

1. `smoke-wmmax` green, and green for the *new* reason — the ack must carry the
   work-area size, not 512.
2. **Mutation M1:** publish `w=` from the work area but keep sending 512 over the
   event channel. The gate must FAIL (this is what catches a compositor whose
   sentinel and whose request disagree — the vacuity trap DDR-973 §6 names).
3. **Mutation M2:** revert `SURFACE_VA_SLOT` to 1 MiB while keeping
   `DIM_MAX = 1024`. This must be caught — by the `_Static_assert` at build time,
   which is the point of tying them.
4. The surface gates that do not maximize — `smoke-surface`, `smoke-evresize`,
   `smoke-drag`, `smoke-resizeall`, `smoke-surfclose`, `smoke-surfdestroy` —
   must be unchanged, since the VA slot and dimension cap moved underneath them.
5. Kernel hash recorded with every measurement (R1), `-Werror` clean, and
   `kernel.bin` still under 1,572,864 B.

## 8. Correction to the queue note on this row

`docs/NEXT_TASK_QUEUE.md` carries, on this item: *"**Read DDR-975 §7 first** —
the client-side resize-ack is the bug, not the WM"*.

That note is **stale in two ways**, and following it would have sent this work
in the wrong direction:

- **DDR-975 §8 already corrected §7.** A second capture failed at a *different*
  arm (the restore click), with `EV_RESIZE_OK` present. §8's own words: §7
  "named a defect where it should have named *one observed stopping point*."
  The queue promoted §7's narrowed conclusion to a standing instruction without
  carrying §8's retraction.
- **Neither §7 nor §8 is about this row.** Both concern `smoke-wmmax`'s
  intermittency (2 failures in ~24 shard-5 runs, 8/8 locally green). The row is a
  *feature* — maximize covers a quarter of the screen — and its blocker is
  `SURFACE_DIM_MAX` and the VA slot pinned to it, which neither section mentions.

The intermittency remains open and is **not** claimed fixed here. This change
does touch its surface area (the requested size, the ack, the second injection's
arming string), so a recurrence must be read against DDR-975 §8 afresh rather
than assumed pre-existing.

---

## 9. MEASURED

Kernel **`92eb02028af0a929`** (was `bb9c6187a30bb0dd`), warning-clean at
`-Werror`, `kernel.bin` **1,098,122 B** against the 1,572,864 B gate
(474,742 B headroom).

### 9.1 The gate is green for the NEW reason

```
[wmmax] compositor asked for 798x728
[wmmax] PASS — PRADYOS_WM_UNMAX id=1
```

798×728 is the Sovereign work area at 1024×768 predicted in §3, arrived at
independently by the code rather than fitted to it: `right = 1024-210 = 814`,
`ax = 8`, `aw = 814-8-8 = 798`; `ay = 6+8 = 14`, `ah = 768-14-8 = 746`,
`mh = 746-18 = 728`. Content occupies (8,32)–(806,760), clearing the agent
panel at x≥814 and the screen bottom at 768.

**4.3× the old area** (798×728 = 580,944 px vs 512×512 = 262,144 px).

### 9.2 M1 — the sentinel and the request must agree

Mutant: publish `w=`/`h=` from the work area, but send `512×512` over the event
channel. Kernel `6c37ae145b6e0aa8` (distinct hash — the build was verified, not
assumed, per DDR-1002 §3's trap).

```
PRADYOS_WM_MAX id=1 w=798 h=728
PRADYOS_EV_RESIZE_OK w=512 h=512        <- the disagreement
make: *** [Makefile:2982: smoke-wmmax] Error 1
```

**Caught.** And this is the exact case the old gate could not see: its assertion
was `grep -q "PRADYOS_EV_RESIZE_OK w=512 h=512"`, which this mutant satisfies.
A compositor that announces one size and requests another **passed** before and
fails now. That is the concrete content of §5's claim.

### 9.3 M2 — the VA slot must move with the dimension cap

Mutant: `SURFACE_VA_SLOT` back to `0x100000` (1 MiB) with `DIM_MAX = 1024`.

```
kernel/syscall/sys_surface.c:38:34: note: expression evaluates to '4194304 <= 1048576'
_Static_assert(SURFACE_BYTES_MAX <= SURFACE_VA_SLOT,
1 error generated.
make: *** [Makefile:620: build/kernel.bin] Error 1
```

**Caught at build time**, which is the point — the failure mode it prevents
(surface N's pages mapped over surface N+1's window, in the compositor, across
process boundaries) produces no panic and no gate sentinel. It would have shipped
as "sometimes a window shows another window's pixels".

### 9.4 Regression — the gates that did NOT change behaviour

The dimension cap and the VA slot moved underneath every surface consumer, so
each was re-run on `92eb02028af0a929`:

| gate | result |
|---|---|
| `smoke-surface` | PASS |
| `smoke-drag` | PASS |
| `smoke-evresize` | PASS |
| `smoke-resizeall` | PASS |
| `smoke-surfclose` | PASS |
| `smoke-surfdestroy` | PASS |
| `smoke-wmmin` | PASS |

Hygiene: `smoke-selftest` PASS, `smoke-shell` PASS (full line: prompt, echo,
help, ls, ps, touch/rm, uname/date/uptime/dmesg/free, redirects, truncate/append,
stderr, N-stage pipes >4 KiB, clean, no panic), `smoke-blkmq` PASS,
`smoke-blk-integrity` PASS, `smoke-rqstress-liveness` PASS,
`ci-shard-check` OK (156 gates / 10 shards / 7 excluded),
`ci-probe-rodata-check` OK (61 ELFs).

### 9.5 What is NOT measured

- **The Manual-mode work area.** `work_area()` branches on the mode and the
  Manual arm is reasoned from `MANUAL_MENUBAR_H`/`MANUAL_TASKBAR_H`, but
  `smoke-wmmax` boots in Sovereign and never exercises it. Recorded as
  unexercised rather than claimed, the same treatment DDR-998 gave its M3 and
  DDR-1004 its SKIP branch. A Manual arm would need the gate to toggle mode
  (Super+M, DDR-992) before clicking the max box.
- **The frame-cost prediction in §6.** 2.2× the blit work per maximized window
  was predicted; nothing here measured frame time. `smoke-wmmax` completed
  inside its 120 s window with the second injection landing, which bounds the
  cost as "not fatal to this gate" and nothing more.
- **`smoke-wmmax`'s intermittency (DDR-975 §8).** One local pass is not a
  measurement of a 2-in-24 CI rate. This change touches that gate's surface area
  and neither fixes nor is claimed to fix it.
