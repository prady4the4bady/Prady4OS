= DDR-937 — the evresize press branch is ALREADY observable; the gate just hides it

**Status:** ACCEPTED. Diagnosability only — no behaviour change.
**Date:** 2026-08-16
**Lineage:** DDR-926 → DDR-929 (regression) → DDR-935 (parse fixed, second
defect exposed) → **DDR-937 (this)**. Method borrowed from DDR-896.

## The question left open by DDR-935

DDR-935 fixed the `rz=` parse; `smoke-evresize` is still 2/3, and the failing
run pressed the **correct** corner without starting a resize. The open question
was *which* of the press-dispatch branches swallowed it.

## The press dispatch is a single if/else chain

`user/compositor.c:1085-1174`, on the button-down edge, in order:

| # | branch | condition | emits |
|---|---|---|---|
| 1 | agent card | `agent_card_hit()` | `PRADYOS_AGENT_TRIGGER` |
| 2 | max box | `max_box_hit()` | `PRADYOS_WM_MAX` / `_UNMAX` |
| 3 | min box | `min_box_hit()` | `PRADYOS_WM_MIN` |
| 4 | close box | `close_box_hit()` | `PRADYOS_WM_CLOSE` |
| 5 | title bar | title rect | `PRADYOS_DRAG_START` |
| 6 | **resize corner** | 14x14 at `(x+w-14, y+h-14)` | *(sets `resizing=1`, silent)* |
| 7 | plain click | fallback | `PRADYOS_MOUSE_OK x y` |

**Every branch except 6 already prints a distinct sentinel.** So a press that
does not resize is already self-identifying in the serial log:

- `PRADYOS_MOUSE_OK <x> <y>` ⇒ branch 7 ⇒ the corner hit-test **missed**, and
  the printed x/y is the coordinate it missed with.
- `PRADYOS_DRAG_START` ⇒ branch 5 ⇒ the press landed on the **title bar**
  instead — the injected point was inside a different surface's title rect.
- `PRADYOS_WM_MIN`/`_MAX`/`_CLOSE` ⇒ branches 2-4 ⇒ it hit a **window button**.
- **none of the above** ⇒ the press never reached the dispatch at all ⇒ the
  button-down edge (`ms.buttons && !prev_btn`) was never observed, i.e. the
  event was dropped or coalesced before the compositor polled.

Those four outcomes point at four different subsystems. No new instrument is
required to separate them.

## Why CI has never shown it

```make
@grep -q "PRADYOS_RESIZE_REQ id=1" build/evresize.log || { \
    echo "[evresize] FAIL — corner drag did not request a resize"; \
    tail -20 build/evresize.log; exit 1; }
```

`tail -20`. The press happens well before the end of a boot log, so every
discriminator above scrolls past unseen. Three CI failures produced no branch
evidence for exactly this reason — the same defect DDR-896 fixed for
`smoke-agent-click`, in a gate that never got the same treatment.

This is the DDR-910 "assume instead of observe" class one level up: the
observation existed and the harness threw it away.

## Decision

Widen both `smoke-evresize` failure dumps to print the press-relevant lines
plus a wide tail, mirroring DDR-896:

```
--- press/geom lines (DDR-937) ---
<PRADYOS_WM_GEOM / MOUSE_OK / DRAG_START / RESIZE_REQ / WM_MIN|MAX|CLOSE
 / EV_RESIZE / AGENT_TRIGGER lines>
--- tail 200 ---
```

`PRADYOS_WM_GEOM` is included so the corner the compositor *published* can be
compared against the coordinate the failing press *used* — if they differ, the
surface moved between publish and injection and the injector's premise is
stale, which is a fifth possibility the dump must not hide.

## Explicitly not doing

No mechanism is proposed and no fix is written. DDR-920/928/932 each named a
mechanism from inference and each was refuted; DDR-935 was a regression I
shipped on a single unrepresentative pass. The next `smoke-evresize` failure
will name its own branch.

## Verification bar

Diagnosability-only, so the bar is that the gate still passes when it should
and the dump appears when it fails. `smoke-evresize` 3x (expect ~2/3 until the
second defect is fixed) and confirm a failing run now prints the new section.
`ci-shard-check` + `sentinel_collision.sh` must stay clean — the dump adds no
new sentinel, it only greps existing ones, which is why this cannot collide.

## Result — and one caught mistake

`sentinel-collision: OK — 159 sentinels` (count unchanged, as predicted),
`ci-shard-check` OK, `ci-probe-rodata-check` OK, `ci-start-align-check` OK,
`make -n smoke-evresize` parses.

The first attempt at the dump did **not** pass. It greped for
`PRADYOS_EV_RESIZE`, and the checker rejected it:

```
COLLISION: 'PRADYOS_EV_RESIZE' is a strict prefix of 'PRADYOS_EV_RESIZE_OK'
```

The checker cannot tell a display-only grep from a gate assertion, and it is
right not to try: a truncated form sitting in a recipe is exactly how a future
edit turns a dump into an assertion that passes on the wrong line. Fixed by
matching the full `PRADYOS_EV_RESIZE_OK`.

Worth recording because the tool caught an error in the very change whose
stated purpose was to stop losing evidence — and because the failure mode it
prevented (an assertion satisfied by a longer sentinel) is the same
false-confidence class as DDR-935's one-run "verification".
