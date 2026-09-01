# DDR-1036 — the click injector cannot tell a live window from a ghost

**Status:** DESIGN (implementation to follow)
**Date:** 2026-09-01
**Queue item:** appended 2026-09-01, third of the three residuals the operator named
**Scope:** gate harness + one compositor print. No kernel change.

---

## §1 — The defect, measured

`tools/qemu_runner/mouse_inject.sh`'s `resolve_geometry` (line 80) takes its
click target from what the compositor *says* it drew — the DDR-910 fix that
removed hardcoded pixels, and which was right. It scans the serial log in
reverse for the newest `PRADYOS_WM_GEOM … title=<T>` and reads the requested
field.

**The serial log is append-only.** A window that has since been destroyed still
has its last `PRADYOS_WM_GEOM` line sitting in that log, and nothing in the line
says the window is gone. So the injector resolves a dead window's last known
geometry and clicks it. **Measured at 45 clicks on a dead window in one capture**
(DDR-1028).

This is the residual DDR-1028 named and did not fix. DDR-1028 fixed the *timing*
(`PRADYOS_INPUT_READY` + `GRACE_SECS` 4 → 12, 6/6 from a pooled ~6/14) so the
click now arrives while `surfacetest`'s window C still exists. It did not fix
the injector's inability to *notice* when a target has gone.

## §2 — Why this is worth fixing rather than living with

It is harness-only — no product impact — but it degrades the harness in the
direction that matters: **it makes a gate's failure message wrong.** DDR-1028
§ records `smoke-wmclose` reporting *"close box click did not close"* about a
window that no longer existed. A gate that misattributes its own failure costs a
session, and this one already has.

It also removes an assumption the harness currently cannot check, which is the
same shape as DDR-910's original fix.

## §3 — The design

**One new compositor line, and one parser rule.**

### §3.1 — `PRADYOS_WM_GONE title=<T> id=<N>`

The geometry publish block (`compositor.c:1439`) already iterates the **live**
surfaces each time it republishes. So the compositor knows exactly which titles
it published last round and which are no longer present. It emits one
`PRADYOS_WM_GONE` per title that has dropped out.

**Why a separate line rather than a field on `WM_GEOM`:** a gone window emits no
`WM_GEOM` at all — that is the whole problem — so the record cannot ride on the
line whose absence is the symptom.

**Appended, never inserted**, per the discipline the `WM_GEOM` block already
documents at length: `drag_inject.sh` isolates fields with `${geom##*dg=}` and
`mouse_inject.sh` scans tokens with `startswith(field + "=")`, so a *new line*
is inert for both until they are taught to read it.

### §3.2 — Newest-line-wins, over both record types

`resolve_geometry` already scans in reverse and takes the newest match, with the
comment *"newest wins: layout can change"*. The rule extends naturally:

> Scan in reverse. The **first** line mentioning `title=<T>` decides. If it is a
> `WM_GEOM`, the window is live — use it. If it is a `WM_GONE`, the target is a
> ghost — do not click.

This handles surface-slot recycling for free: the 16 slots recycle immediately
(DDR-998), and a title that is destroyed and recreated emits `GONE` then a fresh
`GEOM`, so newest-wins reports live, correctly.

On a ghost, `resolve_geometry` returns `False`, which the existing wait loop
(line 141) already treats as "not ready yet" and retries until its deadline —
so a target that is *about* to be recreated is waited for, and one that never
comes back times out with the existing `[inject] TIMEOUT` message rather than
silently clicking empty space.

## §4 — Blast radius, and why it needs stating

`mouse_inject.sh` is used by **seven** gates: `smoke-mouse`, `smoke-wmclose`,
`smoke-wmmax`, `smoke-wmmin`, `smoke-perrestore`, `smoke-surfclose`,
`smoke-agent-click`. All seven must be re-run.

**The regression to watch for is a new timeout**, not a new wrong click: if any
gate legitimately clicks a window *after* its last geometry publish, the new
rule would refuse and that gate would time out. The reason to expect this is
safe is DDR-997, which fixed `PRADYOS_WM_GEOM` being republished only on a
surface-count or focus change — it is now republished continuously, so a live
window always has a `WM_GEOM` newer than any `WM_GONE`. **That is a reason to
expect safety, not evidence of it**; the seven gates are the evidence, and if
one of them times out, that is a real finding about the publish cadence rather
than something to tune around.

## §5 — The gate, and the mutant

**No new gate.** `smoke-wmclose` is already the scenario — `surfacetest`'s window
C self-closes — and adding a gate for a harness property that an existing gate
exercises would be duplication.

**The arm:** the injector prints `[inject] target gone title=<T>` when it
refuses, and `smoke-wmclose` asserts that a click is never issued against a
title whose newest record is `WM_GONE`.

**M1 — delete the `WM_GONE` print from the compositor.** The parser then sees
only the stale `WM_GEOM`, resolves the ghost, and clicks it: the pre-fix
behaviour. This is the mutation that proves the new line is what carries the
information, rather than the parser change appearing to work for another reason.

**M2 — keep the print, ignore it in the parser.** Same observable outcome by a
different route; it separates "the compositor publishes it" from "the parser
acts on it", which §7's dead-arm rule requires, because a single mutant that
defeats both cannot say which half is load-bearing. That is DDR-1033's lesson,
and DDR-1034 §2 applied it; it applies here too.

## §6 — What this does NOT do

- It does not make the compositor's geometry publishing transactional. A window
  destroyed *between* the parser's read and the injector's click is still
  clicked. The window for that is one poll interval, versus the current
  unbounded staleness — a large reduction, not an elimination, and it is stated
  as such.
- It does not touch the kernel, any syscall, or the input path.
- It has no bearing on OPEN-1, OPEN-2, OPEN-12 or OPEN-13.
