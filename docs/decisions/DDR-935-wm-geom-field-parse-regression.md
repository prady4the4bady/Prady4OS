= DDR-935 — I broke `smoke-evresize` by appending a field to `PRADYOS_WM_GEOM`

**Status:** ACCEPTED. **This is a self-inflicted regression, found in CI.**
**Date:** 2026-08-16
**Lineage:** DDR-926 (rz=) → DDR-929 (dg=, the regression) → **DDR-935 (this)**.

## What happened

DDR-926 fixed `smoke-evresize` by publishing the resize corner as `rz=X,Y` on
`PRADYOS_WM_GEOM` and having `drag_inject.sh` read it. Verified 4/4 locally.

DDR-929 then fixed `smoke-drag` by appending **`dg=X,Y` to the same line**.
Verified 3/3 locally, and I re-ran `smoke-evresize` once in that commit's
hygiene — it passed, so I recorded the change as safe.

It was not. CI then failed `smoke-evresize` **twice** (runs 31913149054 and
31913252085), on tips that contained the DDR-926 fix.

## Root cause — a shell parse that reads past its own field

```sh
rz=${geom##*rz=}      # everything after "rz="  -> "6309,8416 dg=4804,5596"
SX=${rz%%,*}          # up to FIRST comma       -> "6309"   correct
SY=${rz##*,}          # after LAST comma        -> "5596"   WRONG - that is dg's Y
```

`${x##*,}` scans to the **last** comma **in the whole remaining line**, so the
moment any field was appended after `rz=`, the Y coordinate was silently taken
from that later field. Measured:

```
OLD parse: SX=6309 SY=5596   (expected SX=6309 SY=8416)
NEW parse: SX=6309 SY=8416
```

The resize press therefore landed ~2800 tablet units too high — nowhere near the
14-pixel corner — so no resize started. Exactly the observed failure.

## Why one local run passed and CI failed twice

The single post-DDR-929 `smoke-evresize` run in that commit's hygiene passed,
and I treated one pass as confirmation. It was luck: the gate's other assertion
path can still be satisfied when the press misses, and a 1-run sample cannot
distinguish that from a real pass. **The 4/4 evidence I cited belonged to the
PREVIOUS commit's code, not to the code I was shipping.** Re-running a
neighbouring gate once after a change is not verification of that change.

## Fix

Isolate the field before splitting, in **both** parsers:

```sh
rz=${geom##*rz=}
rz=${rz%% *}          # <-- cut at the first space: this field only
SX=${rz%%,*}
SY=${rz##*,}
```

`dg=` gets the same treatment even though it is currently last on the line —
being last is not a property worth depending on, and this DDR exists precisely
because that assumption failed once already.

## Rule this establishes

`PRADYOS_WM_GEOM` is an append-only, space-separated `key=value` line with
multiple consumers. Any parser of it MUST isolate its own field before
splitting. Appending a field is otherwise a silent, remote breakage of every
consumer that scans to the end of the line — the failure appears in a gate that
the appending change never touched.

## Verification bar

`smoke-evresize` AND `smoke-drag` 3x each — both consumers, because the fix
touches both parsers and either could regress the other.

## Result — the parse is fixed; a SECOND defect is now visible

```
evresize: observed rz for id=1: start=6309,8416  [evresize] FAIL
evresize: observed rz for id=1: start=6309,8416  [evresize] PASS
evresize: observed rz for id=1: start=6309,8416  [evresize] PASS
drag: PASS  drag: PASS  drag: PASS
```

**The parse defect is closed.** Y is 8416 in all three runs — the DDR-926 value
— where the broken parser produced 5596. `smoke-drag` is 3/3, so the `dg=`
parser did not regress.

**`smoke-evresize` is still 2/3, and that failure is NOT this bug.** Run 1
pressed at the correct corner and no resize started anyway. So there are two
independent defects here and the parse bug was masking the second one: while
every press landed on the wrong row, a residual flake in the press-to-resize
path could not be distinguished from the systematic miss.

This is the DDR-917/918/920/923 "one message, several causes" class again. It is
recorded, not fixed, in this slice — fixing it needs its own capture of a
failing run (what the compositor saw for the press), and shipping the parse fix
now is what makes that capture meaningful. `smoke-evresize` is therefore
**expected to remain intermittent in CI** and is NOT clear for the three-greens
promotion count until the second defect is resolved.

## Correction to the "Why one local run passed" section above

That section assumed the single post-DDR-929 pass was luck against a
systematically-broken press. Given the 2/3 result, the more accurate statement
is that a 1-run sample could not have distinguished the parse bug from the
residual flake in either direction. The methodological point stands unchanged:
one run of a neighbouring gate is not verification.
