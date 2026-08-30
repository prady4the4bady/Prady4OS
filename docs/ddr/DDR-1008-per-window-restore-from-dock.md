# DDR-1008 — Per-window restore from a dock

**Status:** DESIGN. Implements the Group E row "Per-window restore from dock
(DDR-717 restores all)".

---

## 1. What exists, and what is missing

DDR-717 minimize is complete on the *hide* side: the min box sets a bit in
`g_min_mask`, and every compositing and hit-testing loop skips a set bit
(`compositor.c:933, 947, 1123, 1249, 1348, 1453`).

The *restore* side is one keystroke:

```c
else if (c == 'r') {                             /* DDR-717: restore all */
    g_min_mask = 0;
    printf("PRADYOS_WM_RESTORE\n");
```

So a user with three windows who minimizes one and wants it back must un-minimize
**all** of them. There is also no on-screen evidence a minimized window still
exists — it vanishes with nothing left to click.

## 2. Design

A **dock**: a strip of tiles along the bottom, one per minimized window, drawn
as an overlay *above* the windows, present only while `g_min_mask != 0`. Clicking
a tile restores **that** window and leaves the others minimized.

```
DOCK_TILE_W 96   DOCK_TILE_H 24   DOCK_GAP 4   DOCK_MARGIN 8
tile i origin = (DOCK_MARGIN + i*(DOCK_TILE_W+DOCK_GAP), H - DOCK_TILE_H - DOCK_MARGIN)
```

Three decisions worth stating, because each has a cheaper wrong version:

**Tiles are ordered by ascending surface id, not by z-order or focus.**
`SYS_SURFACE_POLL` returns z-sorted, and z changes on every raise. A dock whose
tiles reshuffle when an unrelated window is clicked is both bad UI and an
untestable target — DDR-910's whole finding was that a gate silently encoding
window *creation* order broke the moment fair-share scheduling changed it. Id
order is stable across everything the user or the scheduler can do.

**The dock is an overlay and does NOT shrink the work area.** DDR-1007 made
maximize fill a mode-aware work area. If the dock were part of that area, a
window would resize itself every time an unrelated window was minimized. The
dock draws over whatever is beneath it, like a macOS dock.

**The dock is Sovereign-only.** Manual mode already draws window buttons in its
own taskbar (`render_manual`, `MANUAL_TASKBAR_H`), so a second strip above the
first would be two docks. Wiring Manual's existing buttons to the same restore
path is the Manual answer and is a **separate change** — recorded here as not
done, not silently skipped.

## 3. Publication (§INV.5), and the recompose trap

The gate must click a tile without hardcoding a pixel, so the compositor
publishes, in tablet coordinates via the same `tab_x`/`tab_y` used by
`PRADYOS_WM_GEOM`:

```
PRADYOS_WM_DOCK n=2 id=1 title=BETA tile=4321,31200
PRADYOS_WM_DOCK n=2 id=2 title=GAMMA tile=7532,31200
```

`n=` is repeated on every line **deliberately** — it is what makes the gate
non-vacuous (§4).

**The trap, found by reading the republish condition rather than by a failing
run:** the block that emits `PRADYOS_WM_GEOM` is guarded by

```c
if (ns != composited || cur_focus != last_focus || geom_moved)
```

Minimizing changes **none** of those — not the surface count, not focus, not any
surface's x/y/w/h. `recompose_scene()` repaints but prints nothing. So a dock
line emitted only from that block would never appear after a minimize, which is
the only time it matters. Publication is therefore driven by `g_min_mask`
changing, via a `last_dock_mask` latch, and fires from every site that writes the
mask: the min box, `r`, and a tile click.

The latch also keeps the log from carrying a dock line per frame.

## 4. The gate: `smoke-perrestore`, and why it minimizes TWO windows

The obvious gate — minimize one window, click its tile, assert it came back —
**is vacuous**: `g_min_mask = 0` (DDR-717's restore-all) passes it. That is the
same shape as DDR-973 §6's chain-repeat mutant and DDR-1004's SKIP branch: a test
whose assertion a strictly weaker implementation also satisfies.

So the gate minimizes **two** windows and restores **one**:

1. Click BETA's min box (coords from `PRADYOS_WM_GEOM ... min=`) → `PRADYOS_WM_MIN id=1`.
2. Click GAMMA's min box → `PRADYOS_WM_MIN id=2`, and `PRADYOS_WM_DOCK n=2`.
3. Click BETA's dock tile (coords from `PRADYOS_WM_DOCK ... title=BETA ... tile=`).
4. Assert `PRADYOS_WM_RESTORE_ONE id=1`.
5. **Assert the dock still lists exactly one tile, and it is GAMMA** — i.e. a
   later `PRADYOS_WM_DOCK n=1 id=2`. A restore-all implementation emits `n=0`
   here and fails.

Step 5 is the load-bearing assertion. Steps 1–4 alone would pass on DDR-717.

## 5. Injector change: one new variable, backward compatible

`mouse_inject.sh` hardcodes the line it resolves against:

```python
if "PRADYOS_WM_GEOM" not in ln or ("title=" + geom_title) not in ln:
```

The dock publishes a different line, so the prefix becomes `GEOM_LINE`
(default `PRADYOS_WM_GEOM`). Every existing caller is unaffected because the
default is the current literal. Field isolation is unchanged —
`startswith(field + "=")` already handles `tile=`, and `tile=` cannot
prefix-collide with any existing field name.

## 6. What must be measured

1. `smoke-perrestore` green, with step 5 present in the log (`n=1 id=2` after
   the restore), not merely a passing exit code.
2. **Mutation M1 — restore-all.** Replace the per-id clear with `g_min_mask = 0`
   and keep the `RESTORE_ONE` print. Steps 1–4 must still pass and **step 5 must
   fail** — that is the proof the gate tests granularity rather than restoration.
3. **Mutation M2 — z-order tiles.** Order tiles by poll order instead of id.
   Recorded as *expected not to fail deterministically*: with two minimized
   windows the two orders often agree, so this mutant is a coverage statement,
   not a caught defect. Stated in advance so a pass is not read as proof.
4. `smoke-wmmin` (DDR-717's restore-all gate) must still pass — the dock adds a
   path, it does not replace `r`.
5. Kernel hash recorded with every measurement (R1); `-Werror` clean; kernel.bin
   under 1,572,864 B.

---

## 7. MEASURED

Kernel **`29c792a8b8f3b056`**, warning-clean at `-Werror`, `kernel.bin`
1,098,122 B against the 1,572,864 B gate.

### 7.1 `smoke-perrestore` green, and green through step 5

```
[inject] observed 'PRADYOS_WM_MIN id=1' after 1 click(s)
[inject] geometry for ALPHA: min=4484,3887
[inject] observed 'PRADYOS_WM_DOCK n=2' after 3 click(s)
[inject] geometry for BETA: tile=4996,31955
[inject] observed 'PRADYOS_WM_UNMIN id=1' after 2 click(s)
[perrestore] PASS — PRADYOS_WM_UNMIN id=1, ALPHA still docked
```

The published dock lines show the id ordering doing its job, and show why
DDR-983's per-click re-resolve is load-bearing here:

```
PRADYOS_WM_DOCK n=1 id=1 title=BETA  tile=1793,31955     <- BETA alone: slot 0
PRADYOS_WM_DOCK n=2 id=0 title=ALPHA tile=1793,31955     <- ALPHA takes slot 0
PRADYOS_WM_DOCK n=2 id=1 title=BETA  tile=4996,31955     <- BETA moves to slot 1
```

BETA's tile **moves** when ALPHA is minimized. An injector that resolved once
would have clicked 1793 forever and never restored BETA. It re-resolves before
every click and took the newest line, which is exactly the case DDR-983 was
written for.

### 7.2 M1 — restore-all, the mutant the gate exists to catch

`g_min_mask &= ~(1u << dock_id)` → `g_min_mask = 0`, print unchanged. Kernel
`4f6d76d427c5a42f` (distinct hash, verified — DDR-1002 §3's stale-binary trap).

```
PRADYOS_WM_MIN id=1                                  step 1  PASS
PRADYOS_WM_MIN id=0                                  step 2  PASS
PRADYOS_WM_DOCK n=2 id=0 title=ALPHA                 step 3  PASS
PRADYOS_WM_UNMIN id=1                                step 4  PASS
PRADYOS_WM_DOCK n=0                                  step 5  FAIL
make: *** [Makefile:2988: smoke-perrestore] Error 1
```

**Steps 1–4 pass and step 5 fails** — precisely the asymmetry §4 predicted, and
the demonstration that the gate tests *granularity* rather than restoration. The
obvious one-window gate would have passed this mutant outright.

Kernel restored to `29c792a8b8f3b056` and re-verified by hash after the revert
(§NON-NEGOTIABLE 16: a revert is not verified until the binary is re-checked).

### 7.3 M2 — z-order tiles

**Not run**, and §6 said in advance it would not be run as a pass/fail: with two
minimized windows, id order and poll order frequently agree, so the mutant is not
reliably caught. Recorded as a stated coverage limit rather than presented as a
clean sheet.

### 7.4 Regression and hygiene

`smoke-wmmin` PASS — DDR-717's restore-all keystroke still works; the dock adds a
path rather than replacing `r`. `smoke-selftest` PASS (its case 5 is the
`GLOBAL_FORBIDDEN` non-emptiness meta-test), `smoke-shell` PASS, `smoke-blkmq`
PASS, `smoke-blk-integrity` PASS, `smoke-rqstress-liveness` PASS;
`ci-shard-check` OK (**157** gates / 10 shards / 7 excluded),
`ci-probe-rodata-check` OK (61 ELFs).
