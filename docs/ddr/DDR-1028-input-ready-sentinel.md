# DDR-1028 — `PRADYOS_AMBIANCE_OK` does not mean the pointer is being serviced

Status: **FIXED + measured (6/6 against a pooled ~6/14)**
Corrects DDR-911's sizing of `GRACE_SECS`, and explains a CI failure on `003dec1`
that predates DDR-1026 and DDR-1027.

---

## 1. How this was found, and a claim I had to withdraw

A CI wake reported `smoke-wmclose` failing on shard 1 of `003dec1`. Its
heartbeat carried the DDR-1025 counters:

```
btnedge=46 mpoll=22792 mbtn=9 btn1drain=0 btnhold=21 mpollwin=1
```

46 press edges injected; the syscall reported a button down **9** times; exactly
**one** poll fell inside a press window across all 46. That is DDR-1026's
mechanism in a second gate.

Locally, `b4c2aca` (DDR-1027) failed it and `f238169` (DDR-1026) passed, and I
called DDR-1027 a regression on that basis. **That was wrong, and it was wrong
because N=1 on each side.** Three more runs each inverted it:

| build | kernel | rate |
|---|---|---|
| `f238169` | `56a4c4a35c92cfc5` | 1 PASS / 4 |
| `b4c2aca` | `0d1bcd234707e56d` | 2 PASS / 4 |

`f238169` fails *more*. And the CI failure that started this was on `003dec1`,
which predates both. The gate is a pre-existing intermittent.

## 2. The mechanism

Heartbeats from a failing run, `mpoll` being the cumulative count of
`SYS_MOUSE_POLL` calls:

```
t=500 … t=6000   mpoll=0     <- ring 3 has not polled the pointer ONCE
t=6000                       <- GAMMA's last published geometry; C self-closes here
t=6500           mpoll=2     <- the compositor's FIRST pointer poll
t=7500           mpoll=161
t=8000           mpoll=767   <- the loop accelerates from here
```

and the line ordering in the same log:

```
433  last GAMMA geometry
436  PRADYOS_SURFACE_OK 2      <- GAMMA already out of the live set
443  first PRADYOS_MOUSE_OK    <- the FIRST click to reach ring 3
447  PRADYOS_SURFACE_GONE n=2
```

**The first click to reach the compositor arrived after the target had already
closed itself.** `surfacetest.c` self-closes window C once `GRACE_SECS` has
elapsed from its composited event; the injector had spent that whole grace
clicking into a compositor that was not reading the pointer.

The 45 retries that follow are worse than useless. `mouse_inject.sh`'s
`resolve_geometry()` re-reads the serial log each round and takes the newest line
matching `title=GAMMA` — but the log is append-only, so a dead window's geometry
lives in it forever. The injector cannot tell a live target from a ghost, and
reports `never appeared after 45 click(s)`; the gate then prints **"close box
click did not close"**, which reads as a compositor defect and is not one.

### 2.1 Why the sentinel is the root of it

Every pointer gate's injector waits for `PRADYOS_AMBIANCE_OK`. That is printed at
`compositor.c:1184`, and the line's own comment says what it means: *"loop is
about to start"*. It does not mean the pointer is being serviced, and measured
here there is a **~10 s gap** between the two.

This is the same fact DDR-1025/1026 measured on `smoke-mouse` (`btnedge=3` while
`mpoll=0`), reached from a different gate. `smoke-mouse` survives the gap because
DDR-1026's latch holds the press until someone finally polls. `smoke-wmclose`
cannot be saved that way: its target destroys itself inside the gap.

### 2.2 DDR-911 already fixed this once

`surfacetest.c`'s own comment describes the identical failure —

> *"the second job was never written down, so removing the counter removed it and
> 49 correct clicks hit a surface that had already gone."*

— and fixed it with `GRACE_SECS 4`, sized on *"smoke-wmclose's injector lands its
click in about a second"*. That estimate is what fails. It was measured against a
sentinel that does not mean what it looks like, so nothing in the tree could show
the number was wrong.

## 3. The fix, in two parts, each measured separately

**Part 1 — an honest readiness sentinel.** The compositor prints
`PRADYOS_INPUT_READY` once, from *inside* the branch that has just polled the
pointer successfully, so it cannot be true early. `smoke-wmclose` waits on that
instead of `PRADYOS_AMBIANCE_OK`.

Measured alone (kernel `f36de18e4b2eade0`): **3 PASS / 6.** Better placed but
still a coin flip — with the injector now starting at the right instant, its
first click and C's 4 s expiry land in the *same* heartbeat bucket. Both runs
below are indistinguishable at that resolution:

```
PASS  ambiance t=5500  INPUT_READY t=6000  lastGAMMAgeom t=6000  CLOSE_OK t=6000  WM_CLOSE_REQ t=6000
FAIL  ambiance t=5500  INPUT_READY t=6000  lastGAMMAgeom t=6000  CLOSE_OK t=6000  WM_CLOSE_REQ —
```

**Part 2 — `GRACE_SECS` 4 → 12.** The grace has to outlive the click phase, not
merely start beside it. 12 is derived, not picked: a *passing* run needs 8 press
edges, the injector's round is ~1.2 s, so ~10 s, and 12 covers it while leaving
`smoke-winops` (`TIMEOUT_S=90`, and it needs C to self-close) room to observe the
shrink.

Measured together (kernel `aad7b4c7a2e1a776`): **6 PASS / 6.**

| build | kernel | `smoke-wmclose` |
|---|---|---|
| `f238169` | `56a4c4a35c92cfc5` | 1 / 4 |
| `b4c2aca` | `0d1bcd234707e56d` | 2 / 4 |
| + `INPUT_READY` | `f36de18e4b2eade0` | 3 / 6 |
| + grace 4→12 | `aad7b4c7a2e1a776` | **6 / 6** |

Pooling the three pre-fix binaries is not legitimate for a *rate* (each binds its
own binary, DDR-1009 §8.3), but as a bound on what 6/6 has to beat they sit
around 6/14 ≈ 0.43, and `0.43⁶ = 0.006`. The change is not the dice.

`GRACE_SECS 4` on the current tree is the mutation of part 2, and it has already
been run: that is the `f36de18e4b2eade0` row, 3/6.

## 4. What was deliberately NOT changed

**`smoke-mouse` still waits on `PRADYOS_AMBIANCE_OK`.** Pointing it at
`PRADYOS_INPUT_READY` would make its five clicks land while ring 3 is polling —
and would thereby remove the only coverage DDR-1026's press-edge latch has. That
gate's job is to exercise the dead window; this DDR's fix would hide it.

The other pointer gates (`smoke-drag`, `smoke-agent-click`, `smoke-wmmax`,
`smoke-wmmin`, `smoke-resizeall`, …) are also unchanged. They pass, and
`PRADYOS_INPUT_READY` is now available to any of them that starts to flake.
Re-pointing gates that are green, days from a release, buys nothing and risks
the ones that work.

**The compositor's ~10 s to first poll is NOT fixed, and is not understood.**
`mpoll` goes 2 → 161 → 767 → 1678 across successive heartbeats, so the loop is
running and its early iterations are enormously slow, then accelerate. Why is
not established here, so §NON-NEGOTIABLE 3 forbids a fix. It is a real product
defect on its own terms — a desktop that ignores the mouse for ten seconds after
it has drawn itself — and it is the common cause behind DDR-1025, DDR-1026 and
this DDR. Named, measured, left open.

## 5. Residual: the injector still cannot see a dead target

`resolve_geometry()` will still click a ghost for the full 45 rounds if a target
disappears, and the gate will still call that "close box click did not close".
The honest repair is for the resolver to reject a `PRADYOS_WM_GEOM` line older
than the last `PRADYOS_SURFACE_GONE`, turning a misleading 54-second timeout into
an immediate, correct diagnosis. Not built — it changes shared tooling that eight
gates depend on, and every one of them is green. Recorded for post-1.0.

## 6. Gates

`smoke-wmclose` 6/6; `smoke-winops` and `smoke-surface` PASS (the two that need C
to self-close and to be composited); `smoke-ctrlaltt`, `smoke-mouse`,
`smoke-drag`, `smoke-focus` PASS after the compositor change.
`hygiene_check.sh` ALL THREE PASSED.
