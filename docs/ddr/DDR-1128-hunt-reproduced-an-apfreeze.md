# DDR-1128 — THE HUNT REPRODUCED AN `[apfreeze]`, AND DDR-1123'S PER-CPU CENSUS FIRED FOR REAL FOR THE FIRST TIME

**Status:** artefact captured. **Docs-only** — no code change, no gate,
`kernel.bin` NOT rebuilt (and **could not be**, see §6). **NO FIX, NO MECHANISM
NAMED, OPEN-2 DOES NOT CLOSE** (NON-NEGOTIABLE 3). No open issue moves.

---

## 0. What happened

DDR-1127 recorded the **scheduled** hunt: 60 boots, 0 signals, on the
**pre**-CR0.WP binary. Its §6 said a second, manually dispatched run against the
**CR0.WP** kernel was a separate datum that must not be pooled. **That run came
back `conclusion: failure`, and on this workflow failure means it found
something.** §6 has been corrected at the site: it was already complete when
DDR-1127 was written, and it was not clean.

**One lane of seven failed; six succeeded.** This is the first time the hunt has
produced an artefact of any kind.

---

## 1. The artefact, verbatim

Run `35504467004`, lane 0, run 7 of 10, `kernel_pinned=ca8107ec7f5d8de7`:

```
[campaign] run=7 rc=1 [hb] t=17500 NO-CHURN *** SIGNAL x4 *** cap=…/run-7.log.fail-6189
[apfreeze] cpu=3 ticks=155 rip=0xFFFFFFFF80016D91 cs=0x08 rflags=0x06 if=0
           rsp=0x07FA7938 lvt=0x20030 masked=0 svr=0x1FF swen=1 tpr=0
           isr48=0 irr48=1 pid=11 shot=1
           bt=0xFFFFFFFF80013C01,0xFFFFFFFF80013B48,0xFFFFFFFF800495FA,0x07FA9023
           ticks[0=1500,1=1479,2=1478,3=155]
…shot=2  ticks[0=2000,1=1979,2=1978,3=155]
…shot=3  ticks[0=2500,1=2479,2=2478,3=155]
…shot=4  ticks[0=3000,1=2979,2=2978,3=155]
[campaign] DONE runs=10 signal_runs=1 churn_runs=9 kernel_pinned=ca8107ec7f5d8de7
```

**THE SIGNAL IS `[apfreeze]`, NOT `[ringwalk] RECYCLED`.** The job's error text
names RECYCLED because that string is the generic message; the `SIGNALS` pattern
matches both, and what matched here is four `[apfreeze]` shots. **So this is an
OPEN-2 freeze reproduced in the hunt, not the precondition detector firing** — a
different and larger thing than the instrument was built to catch, and the
distinction matters because DDR-1127 §4's finding (the precondition never fired
in 60 boots) is **untouched** by it.

---

## 2. DDR-1123's census fired for real, and it says ONE frozen CPU

DDR-1123 added the per-CPU tick census to `[apfreeze]` and the handoff recorded
that **its first real fire had not yet happened**. This is it.

`ticks[0=1500,1=1479,2=1478,3=155]` … `ticks[0=3000,1=2979,2=2978,3=155]`:
across four shots spanning 1500 ticks of wall progress, **CPUs 0/1/2 climb
together and CPU 3 is pinned at 155**. So:

* **Exactly ONE CPU is frozen.** This is **not** DDR-1122's two-frozen-CPU shape
  reproduced — that capture had CPU 2 *and* CPU 3 pinned while two climbed.
  DDR-1122's correction (one `[apfreeze]` line is a **latch, not a census**) is
  exactly why that could not have been asserted before this field existed, and
  here the census **confirms** rather than merely fails to contradict.
* `cpu=3` and the pinned index agree, so the latch picked the CPU the census
  independently shows frozen.

**THE RIP IS PINNED ACROSS ALL FOUR SHOTS.** Per `ap_freeze_probe`'s own stated
reason for staying on one victim, a walking RIP would mean the CPU is running and
merely masked; **a pinned RIP means it is spinning.**

---

## 3. What the line establishes WITHOUT resolving a single address

`rflags=0x06` has bit 9 clear, which corroborates `if=0` independently. Then:
`masked=0` (LVT unmasked), `svr=0x1FF`/`swen=1` (LAPIC enabled), `isr48=0` (no
stuck in-service vector), **`irr48=1` — a timer interrupt PENDING AND
UNDELIVERED** — with `tpr=0`. So the CPU is not halted and not starved: **it is
running with interrupts disabled**, and IF is the only thing left blocking
delivery.

**That is the field-for-field shape DDR-981 recorded**, and it is stated as a
**shape and nothing more**. §INV.18 and DDR-1019 both forbid reading a producer
off an offset without resolving against the binary that produced it, and
DDR-1019's whole finding was that two captures sharing an offset in *different*
binaries were *different producers*. **The site is NOT identified here.**

---

## 4. `NO-CHURN` on this run is a CONSEQUENCE, not a vacuous run

`run=7` is flagged `NO-CHURN`, and `churn_runs=9`. Read carelessly that is
DDR-1097 §7.2's vacuity case — a boot that never reached `rqstress_proof` and
therefore tested nothing. **It is the opposite here.** The boot froze a CPU at
tick 155 and the census shows it still frozen at tick 3000; `[smp] rqstress OK`
never printed **because of the freeze**, not instead of it. The other nine runs
all had churn. **A NO-CHURN run that also carries a signal is the one case where
the flag must not be read as "this run tested nothing".**

---

## 5. WHAT THIS DOES AND DOES NOT SAY ABOUT DDR-1126 (CR0.WP)

`ca8107ec7f5d8de7` is the `OPEN2_HUNT=32` build of **`a390eab`**, the CR0.WP
kernel. DDR-1126 §8 said in as many words that it is **not exonerated in
advance** and that if the OPEN-2 signature moves, it is a candidate. So the
question is asked here rather than avoided.

**THE COMPARISON DOES NOT ESTABLISH A REGRESSION, AND THAT IS COMPUTED, NOT
ASSERTED.** 0 signals in 60 boots (pre-CR0.WP, DDR-1127) against 1 in 10 (post):
under the null that the single signal is equally likely to have landed on any of
the 70 boots, **one-sided p = 0.143**. The 1-in-10 exact 95% interval runs to
**39.4%** and overlaps the pre-fix **<4.87%** bound heavily. **Not significant,
and the two are different binaries anyway** (DDR-1127 §5's no-pooling rule cuts
both ways).

**Three further facts push the same direction and none of them is a proof:**

1. **OPEN-2 long predates CR0.WP** — `[apfreeze]` captures run back through
   DDR-1006, 1010, 1019, 1079, 1088, 1099 and the `[schedcheck]` family, on
   kernels with WP clear throughout.
2. **The hunt build deliberately perturbs timing.** `OPEN2_HUNT=32` inserts
   pauses in the DDR-955 ring walk precisely to widen a race. A freeze on a hunt
   build is *expected to be likelier* than on a product build and says little
   about the product binary.
3. **DDR-1126's own 18-gate regression, hash-pinned, was 18/18 `rc=0`**, five of
   them SMP gates at `-smp 4`, and all twenty CI shards on `a390eab` were green.

**NOT EXONERATED EITHER.** *"The diff is elsewhere"* is not an argument
(DDR-1042), CR0.WP is set on **every AP**, and nothing here rules it out. What is
recorded is that **this capture does not decide it**, and that deciding it needs
a hunt on the pre-CR0.WP binary at comparable *n* — which is exactly what
DDR-1127's 60 boots are, and they are clean.

---

## 6. WHAT IS OWED, AND WHY IT IS NOT DONE HERE

**The five addresses are NOT resolved**, and that is a stated gap rather than a
skipped step. §INV.18 requires resolving against **the binary that produced the
capture** — `ca8107ec7f5d8de7`, the `OPEN2_HUNT=32` build of `a390eab` — and
**this container cannot build it**: it came up without the toolchain (`make` dies
at `nasm: No such file or directory`) and without submodules, which were
initialised here but do not supply `nasm`, `clang` or `lld`.

The next session should, in order:

1. Rebuild exactly as the workflow does — `make image`, then
   `touch kernel/proc/sched.c && make image OPEN2_HUNT=32` — and **assert the
   hash is `ca8107ec7f5d8de7` before resolving anything.**
2. Resolve `rip=0xFFFFFFFF80016D91` and the four `bt=` frames **with `python3`,
   never `awk`** (`strtonum()` is a gawk extension and this host's `awk` is mawk
   — DDR-1079's defect, re-paid by DDR-1121).
3. Note that `bt[3]=0x07FA9023` is a **low address near `rsp=0x07FA7938`**, i.e.
   almost certainly stack data rather than a code address; do not force it onto a
   symbol.
4. **Download the uploaded capture** (`open2-hunt-lane-0`, artifact
   `10603519323`, 14-day retention — so **before 2026-10-04**). The job log shows
   only the matched lines plus context; the full 180 s capture is in the artifact
   and will carry the lock dump and the heartbeats DDR-1121/1122 needed.

---

## 7. NOT CLAIMED

* **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE.** One occurrence.
* **NO SITE IDENTIFIED** — §3 gives a field-for-field *shape*, and §INV.18
  forbids turning an unresolved offset into a producer.
* **NOT ATTRIBUTED TO DDR-1126 AND NOT EXONERATING IT** (§5).
* **DDR-1127 IS NOT WITHDRAWN.** Its 60-boot dataset, its bound and its §4
  finding (the *precondition* never fired) all stand and are untouched by this —
  what fired here is a different detector arm on a different binary. **One
  sentence of its §6 was factually wrong when committed and is corrected at the
  site**, not deleted.
* **NO RATE for OPEN-2**, on this binary or any other; 1 in 10 with a 95% upper
  bound of 39.4% is not a rate anybody should quote.
* **`OPEN2_HUNT` not retuned**; no workflow change; no new gate (179),
  `GLOBAL_FORBIDDEN` 77, 79 probe ELFs.
