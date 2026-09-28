# DDR-1029 — the compositor's boot cost is 30 full-screen renders, and DDR-1028 mis-stated the gap

Status: **MEASURED + instrument armed. No fix.**
**Corrects DDR-1028 §2.1 and §4 on two counts.** DDR-1028's *fix* stands; two of
its inferences do not.

---

## 1. What DDR-1028 got wrong

DDR-1028 said two things it had not measured with a clock:

- **§2.1: "a measured ~10 s gap"** between `PRADYOS_AMBIANCE_OK` and the
  compositor's first `SYS_MOUSE_POLL`.
- **§4: "the loop is running and its early iterations are enormously slow, then
  accelerate"**, inferred from `mpoll` climbing 2 → 161 → 767 → 1678 across
  heartbeats.

Both are wrong, and both come from the same mistake: reading **`g_ticks`
buckets as wall seconds**. The figure was 1000 ticks between two heartbeats,
converted to "10 s" by assuming the nominal 100 Hz holds in wall time under
TCG. It does not, and the correct statement was always "1000 ticks".

This instrument stamps `SYS_CLOCK` — real seconds — at four points in the
compositor's startup and first iterations. The correlation against the
heartbeats in the same log:

```
412  [hb] t=5000
414  PRADYOS_LOOPSTAMP i=0 at=post-ambiance s=19183
415  PRADYOS_AMBIANCE_OK
416  PRADYOS_LOOPSTAMP i=1 at=top        s=19183
417  [hb] t=5500
440  PRADYOS_LOOPSTAMP i=1 at=pre-keys   s=19184
441  PRADYOS_LOOPSTAMP i=1 at=pre-mouse  s=19184
442  PRADYOS_INPUT_READY
443  PRADYOS_LOOPSTAMP i=2 at=top        s=19184
...  i=2 and i=3 complete inside the same wall second
455  [hb] t=6000
```

`PRADYOS_AMBIANCE_OK` → first successful pointer poll is **one wall second**,
and lands **inside a single heartbeat interval**. Not ten seconds. And
iterations 1, 2 and 3 all complete within that same second, so the loop's early
iterations are **not** slow.

**DDR-1028's fix is unaffected.** It rests on an *ordering* fact — the injector
began clicking before the compositor had polled the pointer — and on outcomes
that were measured, not inferred: 1/4, 2/4, 3/6, 6/6. A one-second gap is still
a gap the injector was landing inside, and `PRADYOS_INPUT_READY` still removes
it. Only the magnitude and the mechanism-behind-it were wrong.

## 2. Where the time actually goes

Stamping each ambiance step:

```
pre-ambiance   s=19351
DAWN           s=19357   (+6)
DAY            s=19362   (+5)
DUSK           s=19367   (+5)
NIGHT          s=19374   (+7)
post-ambiance  s=19379   (+5)
```

**28 wall seconds, all of it before `PRADYOS_AMBIANCE_OK` and before the loop.**

And it is not five renders. `set_ambiance(idx, frames)` (`compositor.c:990`)
draws a `frames`-step OKLab transition — `render()` + `present()` per step — so
the boot does:

> 4 announce transitions + 1 settle transition = **5 × 6 = 30 full-screen
> 1024×768 renders**, at **~0.93 s each**.

That per-render figure is the number worth carrying. Nothing about the frame
loop is pathological: it is one render costing ~0.93 s in ring 3 under TCG,
performed thirty times before the desktop will answer a mouse.

This is also why every pointer gate's timing is delicate. It is not that the
compositor stalls after it is ready — it is that it takes half a minute to
become ready, and each gate's injector, its readiness sentinel and
`surfacetest`'s self-closing window C are all sequenced against that.

## 3. No fix, and why

The obvious saving is the four announce transitions: 24 of the 30 renders exist
to walk through DAWN/DAY/DUSK/NIGHT before settling on the time-of-day
ambiance. But each emits `PRADYOS_AMBIANCE <name>`, and gates assert those
sentinels.

Cutting the renders while keeping the prints would make every one of those
assertions **vacuous** — a compositor that drew nothing would pass them. That is
exactly the failure DDR-1012 removed from `smoke-horizon` by making its gate
sample real pixels, and DDR-973 removed from `smoke-fat32-multicluster` by
choosing a pattern a chain-repeat could not satisfy.

So the honest options are (a) leave it, (b) reduce `frames` from 6 to something
smaller and re-measure every gate whose timing depends on boot length, or
(c) keep one full transition and make the other three assert on framebuffer
readback rather than on a printf. None is a one-line change days from a release,
and (b) and (c) both move timing that eight gates currently pass against.

**Left alone. Measured, named, and cheap to revisit** — the instrument stays
armed, so the next session can see the cost without rediscovering it.

## 4. The instrument

`loopstamp()` prints `PRADYOS_LOOPSTAMP i=<iter> at=<phase> s=<secs>` and is
bounded to the first `LOOPSTAMP_ITERS = 3` loop iterations — eleven lines per
boot. Unbounded, it would print thousands of lines a second and slow the loop it
measures, which is the reason DDR-941 made `PRADYOS_BTN_STATE` print on change
only.

`SYS_CLOCK`'s one-second resolution is not a compromise here: the quantity being
measured is ~28 s, and whole seconds answer it. A finer clock would add a vDSO
dependency to a question seconds already settle. It *is* too coarse to separate
iterations 1–3 from each other — that is recorded as a limit, not claimed as a
result.

## 5. Gates

`smoke-wmclose` PASS on the instrumented kernel `086fb267171c136b`;
`hygiene_check.sh` all three PASSED.
