= DDR-929 — smoke-drag must observe the title-bar point, not hardcode it

**Status:** ACCEPTED. **Date:** 2026-08-16
**Lineage:** DDR-910 → DDR-926 (rz=) → **DDR-929 (this)**.

## Evidence, including an A/B that cleared my own change

`smoke-drag` failed in CI 31907631454 shard 1 ("drag did not start on the title
bar"). Because that gate uses `drag_inject.sh`, which DDR-926 had just modified,
the first question was whether I had regressed it. A/B, two distinct scripts:

| arm | injector | result |
|---|---|---|
| A | pre-DDR-926 (`d00bee0`) | **0/3 PASS** |
| C | current (DDR-926) | **2/3 PASS** |

So the flake **predates** DDR-926 and that change slightly improved it. Not a
regression — a pre-existing instance of the same hardcoded-coordinate class.

## Root cause

The gate relied on `drag_inject.sh`'s built-in default `SX=4800 SY=5546`, a
hand-tuned pixel. The compositor already publishes each surface's geometry
(`PRADYOS_WM_GEOM`, DDR-910/894) but carried no drag point, so the gate had
nothing to observe.

## The fix, and the measurement that corrected it

`PRADYOS_WM_GEOM` gains `dg=X,Y`. The first attempt used `x + 20` and measured
**0/3 PASS** — worse than the hardcode. The reason is in the injector's own
comment: these windows are narrow (w~64), so the three title-bar boxes occupy
`x+20..x+64` and a naive `x+20` lands **on the max box**, which is checked
before the drag fallback.

The point is therefore derived from the same expression as the boxes: the
leftmost box is `x + w - 3*CLOSEBOX - 8`, so the drag strip is `x` to that edge
and the safe point is its **midpoint**. Measured `dg` X = 4804 — essentially the
old hand-tuned 4800, but now computed rather than guessed, so it tracks any
window size or position.

Result: **3/3 PASS**.

## Verification

3/3 smoke-drag; `smoke-evresize` re-checked rc=0 (shares the injector);
build warning-clean; sentinel-collision OK (159); three `ci-*-check` PASS;
`smoke-blkmq`, `smoke-rqstress-liveness`, `smoke-blk-integrity` all rc=0.
