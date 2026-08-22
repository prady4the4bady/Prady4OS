# DDR-975 — `preempt=` flat is not a freeze when `cur=` is a yield-looping client

Status: ACCEPTED (decoder correction + gate reclassification). No behavioural
change. Number verified free in **both** `docs/ddr/` and `docs/decisions/`
(§INV.4).

**Refines:** DDR-947's heartbeat decoder.
**Reclassifies:** the `smoke-wmmax` CI failure at `83554f9`, and the "preempt
frozen" half of `CLAUDE.md`'s smoke-agents row.

---

## 1. The artefact

CI run `32585190577`, shard 5, head `83554f9`, job `97060387734`:

```text
[wmmax] FAIL — client did not honor maximize
PRADYOS_BTN_STATE buttons=1 prev=0 x=482 y=16
PRADYOS_MOUSE_OK 482 16
PRADYOS_BTN_STATE buttons=0 prev=1 x=482 y=16
[hb] t=7000  … btnedge=10 rqdepth=10 rqcpus=1 rqq=1 rqpres=1
             pmmfree=26462 pmmtot=28630 preempt=1826 supp=0 curpid=18 cur=COMPOSIT.ELF
[hb] t=7500  … preempt=1826 supp=0 curpid=18 cur=COMPOSIT.ELF
…
[hb] t=11500 … preempt=1826 supp=0 curpid=18 cur=COMPOSIT.ELF
```

`preempt` is pinned at 1826 for **4,500 consecutive ticks** (45 s at 100 Hz)
while the heartbeat keeps printing. On its face that is a scheduler that has
stopped preempting — which is how `CLAUDE.md` reads the same shape for
smoke-agents ("preempt frozen, `rqdepth=11`").

**The commit is docs-only.** `83554f9` changes exactly one file,
`docs/ddr/DDR-974-b3-is-not-virtio-blk.md`. It cannot alter `kernel.bin`. So
whatever this is, it is not a regression from the diff it failed on.

## 2. What DDR-947's decoder says, and where it stops short

DDR-947 (`idt.c:246`) wrote the decoder:

> `preempt=` climbing while `supp=` stays flat means `schedule()` IS being
> called and not switching; `supp=` climbing means `g_in_switch` is suppressing
> it; **both flat means the tick never reaches the preempt point.**

Here both are flat, so by that decoder the tick never reaches the preempt point.
That is correct as far as it goes. What it does not say is **why**, and the
benign reason is the one that applies here.

## 3. The arithmetic

Three facts from the source, none of them a hypothesis:

1. **`sched_tick()` runs BEFORE the heartbeat print.** `timer_tick()` calls
   `sched_tick()` at `idt.c:167`; the `(now % 500)` heartbeat block starts at
   `idt.c:172`. So 4,500 heartbeat-bearing ticks are 4,500 entries into
   `sched_tick()`, and `current_thread` is plainly non-NULL (`curpid=18` was
   printed from it), so the `if (!current_thread) return;` guard did not fire.

2. **`preempt` (`g_preempt_try`) increments on exactly one branch**
   (`sched.c:1315-1327`):

   ```c
   if (current_thread->quantum > 0)
       current_thread->quantum--;
   if (current_thread->quantum == 0) {
       current_thread->quantum = current_thread->quantum_reset;
       …
       __atomic_add_fetch(&g_preempt_try, 1, __ATOMIC_RELAXED);   /* preempt= */
   ```

   So `preempt` flat across 4,500 ticks means precisely: **`quantum` never
   reached 0.**

3. **`yield()` resets it** (`sched.c:1341`):
   `current_thread->quantum = current_thread->quantum_reset;` — on every call.

And `QUANTUM` is **2** ticks (`sched.c:16`, 20 ms at 100 Hz), while
`user/compositor.c:1261` calls `SYS_YIELD` on **every iteration of its main
event loop**, unconditionally, after `cadence_tick()`. (There is a second yield
at `:680`, inside `present()`'s GPU-busy retry.)

A loop iterating in microseconds resets `quantum` to 2 thousands of times
between two 10 ms ticks. Each tick takes it 2 → 1. **It can only reach 0 if two
ticks land with no yield in between** — i.e. only if the compositor blocks for
>20 ms inside one loop body. In steady-state polling that never happens.

**Therefore `preempt=` is expected to be flat, indefinitely, whenever
`cur=COMPOSIT.ELF` is in its poll loop.** That is arithmetic from `QUANTUM=2`,
the unconditional per-iteration yield, and the reset inside `yield()` — not an
inference from this capture.

## 4. What follows

**`preempt=` counts INVOLUNTARY preemptions only.** A thread that yields
voluntarily faster than its quantum expires is scheduled constantly and preempted
never. `rqdepth=10` in the same capture is consistent with that: `yield()` calls
`schedule()`, so the other ten runnable threads were being picked all along. A
wedged scheduler and a busy yield loop produce the *same* flat `preempt=`, and
this field alone cannot tell them apart.

**The real `smoke-wmmax` failure is the one its own message states:** *"client
did not honor maximize"*. The compositor received the click — `btnedge` reached
10 and `PRADYOS_BTN_STATE` shows a clean press/release pair at x=482 y=16 — and
did not maximize the window. That is compositor behaviour, and it belongs with
the open Group E item *"Window maximize at real display size — DDR-719 caps at
512×512; lift to real geometry"*, not with any scheduler or SMP defect.

**This also weakens the smoke-agents row.** `CLAUDE.md` records that signature as
"`rqdepth=11`, two sentinels missing" with preempt frozen. If that capture's
`cur=` was likewise a yield-looping ring-3 client, the "preempt frozen" half was
never evidence of a freeze. It does not change ITEM 2's outcome — that was closed
as *not reproduced* on the separate ground that the DDR-968 witness never printed
(there is no red artefact at all) — but it does mean the remembered signature is
softer than it reads, and a future session should not treat flat `preempt=` as
corroboration.

## 5. What this does NOT establish

That the compositor *was* in its yield loop during this capture. `cur=COMPOSIT.ELF`
with a healthy heartbeat and a live `rqdepth` is consistent with it, and §3 shows
the yield loop is sufficient to produce the observation — but sufficiency is not
proof of occurrence. What §3 does establish, and what matters for the decoder, is
that **flat `preempt=` with a yield-looping `cur=` carries no information about
scheduler health.** Diagnosing an actual freeze needs a field that a voluntary
yield cannot pin: `pc->ticks`, `rqmiss`/`ubcas`/`ubrq`, or `curpid` changing.

## 6. Recommended instrument change (NOT applied here)

Add a per-CPU **voluntary-yield** counter beside `g_preempt_try`, printed as
`yld=`. `preempt=` flat with `yld=` climbing is a busy client; both flat with the
heartbeat alive is the real "tick never reached the preempt point". One counter
separates the two cases that currently look identical.

Not applied in this DDR: it touches `sched.c`, and a 20× `smoke-rqstress`
campaign (DDR-974 §3) is measuring the current `kernel.bin`. Changing the kernel
mid-campaign would put two binaries under one measurement, which R1 forbids.

---

## 7. Addendum — what the wmmax failure actually narrows to

Read against the recipe (`Makefile`, `smoke-wmmax`), the capture says more than
"the gate failed". The recipe asserts three strings in order, each with
`|| { echo …; exit 1; }`:

```make
@grep -q "PRADYOS_WM_MAX id=1"              … || { echo "[wmmax] FAIL — max box click did not maximize"; … }
@grep -q "PRADYOS_EV_RESIZE_OK w=512 h=512" … || { echo "[wmmax] FAIL — client did not honor maximize"; … }
@grep -q "PRADYOS_WM_UNMAX id=1"            … || { echo "[wmmax] FAIL — restore click did not un-maximize"; … }
```

CI printed the **second** message. Because the first check exits on failure, its
having been passed is implied: **`PRADYOS_WM_MAX id=1` was present.** So

- the click landed on the max box and **the window manager did maximize**; and
- the **client never emitted `PRADYOS_EV_RESIZE_OK w=512 h=512`.**

(The absence of `PRADYOS_WM_MAX` from the `tail -20` in the log proves nothing
either way — that line is emitted early, far outside a 20-line tail. The ordering
of the assertions is the evidence, not the tail.)

This also explains why the run consumed its whole 120 s window rather than
failing fast. The **second** mouse injection is armed on that very string:

```make
@ABSX=15424 ABSY=725 bash tools/qemu_runner/mouse_inject.sh … "PRADYOS_EV_RESIZE_OK w=512" &
```

No `EV_RESIZE_OK` ⇒ the restore click is never injected ⇒ `PRADYOS_WM_UNMAX`
could not have appeared either. The gate reports the **first** failure of a
cascade, not three independent ones.

So the defect to chase is narrow: the surface client's handling of the WM's
resize event (the DDR-718 "client re-commits with KEEP" path), not hit-testing,
not the injector, and not the scheduler.

**A separate §INV.5 violation, noted but not the cause here.** Both injections
use hardcoded absolute coordinates — `ABSX=5311 ABSY=5588` and
`ABSX=15424 ABSY=725` — where §INV.5 requires geometry to come from
`PRADYOS_WM_GEOM` fields. That did not cause this failure (`WM_MAX` fired, so the
first click was on target), but it is exactly the latent fragility the invariant
exists to prevent: DDR-719 §D2 already had to relocate `smoke-drag`'s hardcoded
click once when the title-bar box order changed. Worth fixing on its own merits,
separately from the resize-ack defect.


---

## 8. CORRECTION to §7 — a second occurrence fails at a DIFFERENT arm

`smoke-wmmax` failed again in CI (run `32598036823`, shard 5, head `ff56d47`).
The capture does **not** match §7:

```text
[wmmax] FAIL — restore click did not un-maximize      <- the THIRD assertion
PRADYOS_WM_MAX id=1                                    present
PRADYOS_EV_RESIZE_OK w=512 h=512                       PRESENT this time
```

§7 concluded, from the single capture then available, that the defect was "the
surface client's handling of the WM resize event" and that the run consumed its
whole window because the second injection is armed on `EV_RESIZE_OK`. **That is
not what this occurrence does.** Here `EV_RESIZE_OK` *is* emitted, the second
injection therefore *did* fire, and the failure is the **restore/un-maximize**
click producing no `PRADYOS_WM_UNMAX id=1`.

So the gate has **at least two distinct failure points**, and §7 described only
the first. §7 is not retracted — its reading of that capture is still correct,
and its reasoning about the assertion ordering still holds — but its scope was
too broad: it named a defect where it should have named *one observed stopping
point*. Two captures, two different arms.

### 8.1 Leading candidate for the restore arm — the §INV.5 violation

§7 already flagged that both injections use hardcoded absolute coordinates and
called it "latent fragility ... not the cause here". For **this** arm it is a
live candidate, because the restore click is aimed at a box that has *moved*:

- DDR-719 §D2's own description of the gate is "a second injection **at the
  relocated box**" — the window was just resized to 512×512, so its title bar,
  and every box on it, is somewhere new.
- The gate hardcodes `ABSX=15424 ABSY=725` for that second click.
- The same failing capture prints the live geometry:
  `PRADYOS_WM_GEOM id=1 title=BETA close=16335,726 min=15887,726 …` — i.e. the
  authoritative coordinates were *right there in the log*, unread by the gate.

§INV.5 exists for exactly this: *"Geometry in gates: `PRADYOS_WM_GEOM` fields.
Never hardcoded pixel coords."* A hardcoded restore coordinate that lands on the
max box only when the post-maximize geometry happens to agree with a constant
would fail exactly like this — intermittently, on the third assertion, with the
first two passing.

**Not confirmed, and it did not reproduce.** `smoke-wmmax` run locally **8/8
pass**, zero failures — so there is no local capture in which to test the
hypothesis. It predicts the restore click misses a relocated box; nothing in
either CI capture directly shows where that box was at click time.

**Therefore no fix ships here**, and specifically the injector is *not* rewritten
on this reasoning. Rewriting a gate's coordinate scheme against a failure that
cannot be observed would mean validating the change only against the passing
case — which proves the gate still passes, not that the defect is gone. That is
the shape of every attribution this project has had to retract (DDR-966,
DDR-969, DDR-973, and §7 above, one section earlier in this very DDR).

**What is nonetheless true independent of this defect:** the hardcoded
coordinates violate §INV.5, which exists precisely because DDR-719 §D2 already
had to relocate `smoke-drag`'s hardcoded click once when the box order changed.
That is worth fixing on its own merits, as an invariant repair rather than a bug
fix, and it should be done when it can be validated — i.e. alongside a
reproduction, or as a deliberate harness change with its own before/after run.

**Rate, for whoever picks this up:** 2 CI failures observed across ~24 shard-5
executions, at two *different* assertions, with 8/8 and (§7) same-SHA green
siblings. Any campaign needs to be long enough to see both arms.
