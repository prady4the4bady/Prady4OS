# DDR-989 — `smoke-evresize` / `smoke-agentpanel`: vruntime is sampled, so a sub-tick yielder is never charged

**Status:** ROOT-CAUSED AND FIXED (§9). The §3 sampling hypothesis is REFUTED by
its own §4 test; the real cause is a torn double read of `vt_in`, proven in the
disassembly. Locally reproducible before the fix; 8/8 clean after.
**Artefacts:** 2 independent CI captures (below). Never reproduced locally.
**Supersedes nothing.** Relates: DDR-947 (the `preempt=`/`supp=` procedure),
DDR-968 (the smoke-agents witness), task #26.

---

## 1. Two captures, one signature

| | `smoke-evresize` | `smoke-agentpanel` |
|---|---|---|
| SHA | `3f6dbff` (shard 0) | `cb69da4` (shard 6) |
| `preempt=` | 1702, **flat** t=4000..11500 | 1408, **flat** t=11500..14500 |
| `supp=` | 0, flat | 0, flat |
| `rqdepth=` | 10, pinned | 10, pinned |
| `rqcpus=` | 1 | 1 |
| `curpid=` | alternates 18 `COMPOSIT.ELF` / 42 `AETHERD` | alternates 18 `COMPOSIT.ELF` / 42 `AETHERD` |
| `ymask` | +3.24M over 7500 ticks (**~432/tick**) | +1.6M over 3000 ticks (**~533/tick**) |
| `net_skip`/`net_rxdrop` | (no reader yet) | **0 / 0** |

Same two pids, same pinned depth, same frozen counters, two different gates,
two different commits. This is one defect, not two.

## 2. What is ruled OUT

**lwIP is exonerated.** The DDR-988 §5 counters were added for exactly this and
answered on their first CI failure: `net_skip=0` means the timer never once
found `g_net_lock` contended, and `net_rxdrop=0` means nothing was dropped. The
stall is not lwIP contention and not the deferred-work path.

**Not a DDR-988 regression.** The same signature is present on `3f6dbff`, which
predates the deferred-work change, and CLAUDE.md records it at `9231eab` long
before that (DDR-968 §1, `rqdepth=11`).

**Not the DDR-981 AP freeze.** No `[apfreeze]`, no `compl wait timeout`, and
`g_ticks` advances throughout.

## 3. The mechanism

Two facts, each verifiable by reading, that compose into starvation.

**(a) The picker is vruntime-based, not FIFO.** `rq_pop` (`sched.c:408`) does
`fair ? rq_unlink(q, fair) : rq_take(q)`, and `fair_candidate` returns the
`THREAD_READY` entry with the **smallest `dbg_vruntime`**. Note the comment
block above it still calls FIFO "what the REAL (FIFO) picker chose" — that is
**stale**, and it is why this was not spotted earlier: reading the comment tells
you the queue is fair by construction, and it is not.

**(b) vruntime is SAMPLED at the timer tick.** `sched_charge_elapsed` charges a
real rdtsc delta — but its only caller is `sched_dbg_charge`, and that has
exactly one call site: `sched_tick` (`sched.c:1310`), against
`current_thread`. So a thread is charged **only if it happens to be current at
a 100 Hz sample instant.**

Compose them. A thread that yields sub-tick is essentially never the current
thread when the tick fires, so `dbg_vruntime` never advances, however much CPU
it actually consumed. `fair_candidate` picks smallest vruntime. Therefore:

1. The system yields **~432/tick** (evresize) and **~533/tick** (agentpanel) —
   two to three orders of magnitude above the 100 Hz sampling rate, so yields
   land overwhelmingly between samples rather than on them.

   > **CORRECTION (DDR-993).** This step read "Pids 18 and 42 yield ~1074 times
   > per tick", and BOTH halves of that were wrong.
   >
   > **(a) The arithmetic.** 1074 is `3.24M / 3000` — `evresize`'s numerator
   > divided by `agentpanel`'s span. The windows differ: `evresize` is
   > t=4000..11500 (7500 ticks) and `agentpanel` is t=11500..14500 (3000
   > ticks). Neither gate ever measured 1074. The §1 table now carries both
   > denominators inline so the two columns cannot be crossed again — which is
   > §NON-NEGOTIABLE 17 applied to this document's own numbers, having quoted
   > that rule at other people's.
   >
   > **(b) The attribution.** `ymask` is the DDR-981 **system-wide** counter of
   > yields taken with `RFLAGS.IF` clear. It is not per-pid, so it cannot say
   > that pids 18 and 42 yielded anything. Attributing it to them assumed the
   > conclusion — that those two threads monopolise the CPU — which is what §3
   > is trying to establish.
   >
   > What survives is weaker and still sufficient for a hypothesis: `curpid=`
   > alternates between exactly those two pids across every sample in both
   > captures, so they *are* current at essentially every sample instant, and
   > the aggregate yield rate is far above 100 Hz. That is consistent with the
   > mechanism. It does not measure either thread's residency, and §4's
   > confirming measurement — a **per-thread** yield count and `dbg_vruntime`
   > readout — is what would settle it. Until that runs, this remains a
   > hypothesis, exactly as the status line says.
2. Their vruntime stays frozen near zero. Every other `THREAD_READY` thread has
   been charged at least once and so is strictly larger.
3. `fair_candidate` returns one of those two on **every** pick. The ten queued
   threads can never win.
4. Each voluntary switch gives the incoming thread `quantum_reset`, so
   `current_thread->quantum` never reaches 0, `g_preempt_try` never increments
   (`sched.c:1331`), and the timer preemption that would otherwise break the
   cycle never fires.

Step 4 is why the starvation is *self-reinforcing* rather than transient, and
it is exactly what DDR-947's procedure names: `preempt` flat AND `supp` flat
means "the tick never reached the preempt point". It does not reach it because
the quantum is reset faster than it is decremented.

This is a third variant of the H1/H2 pair recorded at `sched.c:200`. H2 was
"threads ENTER at a stale low vruntime". This is worse: they are never charged
at all, so no entry clamp would fix it.

## 4. What would confirm it — and what would refute it

The mechanism is read from source, and the captures are consistent with it, but
consistency is not proof. §NON-NEGOTIABLE 3 needs the mechanism tied to the
artefact, so the instrument must print, at the stall:

- `dbg_vruntime` for the two running pids AND for the queue head, per heartbeat.
- `dbg_picks` per thread — the count already exists (`rq_pop` bumps it).

**Confirms:** pids 18/42 show `dbg_vruntime` frozen (or advancing far slower
than wall time) while `dbg_picks` climbs, and the ten queued threads show
strictly larger vruntime with `dbg_picks` flat.

**Refutes:** if 18/42's vruntime advances normally and is simply *lower*, the
cause is weighting or entry clamping (H1/H2), not sampling — an opposite fix.
If `dbg_picks` climbs on the starved threads too, they are being picked and the
stall is downstream of the scheduler entirely.

That is one heartbeat extension, and it is cheap. It must land before any fix.

## 5. Candidate fixes — NOT to be applied before §4

Recorded so the next session does not re-derive them, explicitly **not**
endorsed yet:

1. **Charge on switch-out.** Call `sched_charge_elapsed(prev)` in the switch
   path, so time is accounted at every context switch rather than sampled. This
   addresses the mechanism at its source. Cost: an rdtsc per switch on a hot
   path — must be measured, not assumed (R17: total AND per-switch).
2. **Charge the yielder in `yield()`.** Narrower, cheaper, but only fixes the
   yield route; any other sub-tick switch path keeps the bias.
3. **Floor-clamp on pick.** Bound how far below `g_dbg_floor` a candidate may
   sit. Treats the symptom and interacts with H2's clamp.

(1) is the honest fix for the mechanism as stated. (2) is what the deadline
argues for. That trade is a real decision and belongs to the operator, not to a
session that is mid-recovery on another PR.

## 6. Scope

**Deliberately not fixed in PR #13.** That PR is the DDR-987/988 recovery for
`dev/phase1`; this is an unrelated scheduler defect that predates it. Widening a
recovery PR with a scheduler change — unmeasured, on a hot path, days from a
release — is how PR #12 went red. This is task #26 and needs its own change.

It is also **not** a reason to hold PR #13: the defect is present on the base
branch and on `main`, so PR #13 neither causes nor worsens it.

## 7. Standing correction

The stale "REAL (FIFO) picker" comment at `sched.c:~220` should be corrected in
whichever change lands first that touches that file. It actively misleads:
anyone reasoning about starvation from that comment will conclude the queue is
round-robin and rule out exactly the mechanism that is occurring.


---

## 7. §4's instrument is live (2026-08-24), with a healthy baseline

A third CI capture arrived — run 32707039014, shard 0, `smoke-evresize`,
`858a721` — matching this DDR's own artefacts closely: `preempt=1850` frozen
across t=9500..11500, `rqdepth=10`, and `curpid` alternating **18
(COMPOSIT.ELF) / 42 (AETHERD)**, the same two pids §2 names. No `[yieldstall]`
line, so it is not the DDR-994 `mnt_lock` stall.

§4 said the instrument "must land before any fix". It has: `sched_vr_sample()`
(`sched.c`) feeds five new heartbeat fields — `curvr`, `curpk`, `hpid`,
`headvr`, `headpk` — giving vruntime and pick-count for the running thread and
for the first READY thread it is passing over. It REPORTS ONLY; no scheduling
decision changes, and the fix in §5 stays unapplied.

**Healthy baseline, measured now** (kernel `35d5c127579cb48a`, `smoke-yieldstall`,
a boot with no stall):

```
curvr=1652563 curpk=17857  hpid=22 headvr=1652536 headpk=1947
curvr=2912327 curpk=50621  hpid=22 headvr=2912203 headpk=25877
```

Two properties make the comparison sharp:

1. `curvr` and `headvr` track each other to within ~0.008% (1,652,563 vs
   1,652,536). Vruntime is being charged to the running thread.
2. `headpk` climbs hard (1,947 -> 25,877). The queued thread is being picked.

So §4's three outcomes are now separable against real numbers rather than
against an assumption about what "normal" looks like:

| observation at the stall | verdict |
|---|---|
| `curvr` frozen while `curpk` climbs; `headvr` > `curvr`; `headpk` flat | **CONFIRMS** sampling starvation (§3) |
| `curvr` advancing normally, merely lower than `headvr` | **REFUTES** — weighting/entry clamping (H1/H2), an opposite fix |
| `headpk` climbing too | **REFUTES** — queued threads are picked; stall is downstream of the scheduler |

The stall has never reproduced locally and did not here either (`smoke-evresize`
passes locally and finishes before the first heartbeat, so it emits no sample at
all). The data therefore comes from the next CI recurrence. Nothing is fixed
until it arrives.


---

## 8. §4 ran. The hypothesis this DDR was built on is REFUTED.

**And the stall reproduced LOCALLY** (`smoke-evresize`, kernel
`48c6e69484e2683f`), which §1 recorded as never having happened. That alone
changes the economics of everything below — it no longer needs a CI lottery.

### 8.1 The data

Consecutive heartbeats, one boot, chronological:

```
curvr=1819383            curpk=4534     hpid=18  headvr=1821743             headpk=13
curvr=13510798893996098  curpk=1233     hpid=0   headvr=18014398511325305   headpk=4460
curvr=17676247155079426  curpk=29818    hpid=40  headvr=18014398511337535   headpk=1447
curvr=17676247148374979  curpk=125087   hpid=40  headvr=18014398511337535   headpk=1447
...                                              headvr=18014398511337535   headpk=1447
```

### 8.2 Applying §4's rule, unmodified

§4: *"Refutes: if 18/42's vruntime advances normally and is simply lower, the
cause is weighting or entry clamping (H1/H2), not sampling — an opposite fix."*

`curvr` **advances** across every sample and is simply **lower** than `headvr`.
It is not frozen. **That is §4's refutation clause, met exactly.** The §3
sampling story — a sub-tick yielder never charged — does not describe this boot,
and the §5 candidate fixes derived from it must not be applied. Writing the
refutation criterion down before looking is what makes this a result rather than
a rationalisation.

The starvation itself is real and confirmed: `headpk` is **pinned at 1447**
across every subsequent sample while `curpk` climbs past 1.7 million. Pid 40 is
never picked. `rqdepth=10`.

### 8.3 The mechanism that IS measured

The first sample is **healthy**: `curvr=1,819,383`, `headvr=1,821,743` — within
0.13%, exactly the §7 baseline shape. Between sample 1 and sample 2 the whole
vruntime space jumps by **ten orders of magnitude**, to ~1.8e16 ≈ **2^54**. It
does not creep there; it arrives in one step.

`sched_charge_elapsed` accumulates `((d >> 10) * 1024) / w` where `d` is a **TSC
cycle delta**, so vruntime ~= cycles/1024 at default weight. `curvr = 1.77e16`
implies ~1.8e19 cycles charged in a single event — that is 2^64-scale, i.e. one
charge with a `d` near the full width of the counter, not real elapsed time.

`g_dbg_floor` is a monotonic maximum (`sched.c:257-258`), and threads are clamped
to it on create and wake (`:138-142`). So one poisoned charge raises the floor to
~2^54 permanently, and thereafter:

- pid 40 sits at **exactly** the floor, `headvr=18014398511337535`, unchanged
  forever — it is never charged because it is never picked;
- the running threads sit ~3.4e14 **below** it, having been clamped at a slightly
  earlier floor;
- `fair_candidate` picks the LOWER vruntime, so the incumbents win every time;
- at the observed ~8e7 per 500 ticks, closing a 3.4e14 gap needs ~2 million
  ticks. Never, in gate terms.

**That is the starvation, and it is now measured rather than hypothesised.**

### 8.4 What produced the poisoned charge is NOT established

Two obvious candidates were checked and **both are refuted** — recorded so they
are not re-tried:

- **`dbg_vruntime` uninitialised** (§NON-NEGOTIABLE 10). No: it is set to
  `g_dbg_floor` at `sched.c:1010`.
- **`vt_in` uninitialised.** No: set to 0 at `sched.c:1007`, and
  `sched_charge_elapsed` returns early on `!vt_in`, so the first charge is
  skipped by design.

A remaining hypothesis, NOT established and not to be acted on without
measurement: `vt_in` is stamped on one CPU (`sched.c:560`) and differenced on
another (`:253`), and TSCs need not agree across vCPUs under TCG. The `now >
vt_in` guard only rejects a *backwards* delta; a forwards jump of arbitrary size
passes straight through. Confirming that needs the raw `d` and the two CPU ids
at the charge, which is one more instrument — the same discipline as §4.

### 8.5 Consequence for the sibling failures

DDR-994 §8.4 catalogued a "preempt-frozen" signature across `smoke-evresize`
(shard 0), `smoke-invariants` (shard 8) and `smoke-poweroff` (shard 5). The
evresize instance is now explained: preemption is not broken, the fair picker is
correctly choosing the same low-vruntime thread every time. Whether the other two
share this cause is **not** established — they were not sampled. Do not merge
them into this row without their own `curvr`/`headvr` numbers.


---

## 9. ROOT CAUSE: `vt_in` was read twice, and the second read could be newer

### 9.1 The artefact

§8.4's instrument caught the poisoned charge on its first local reproduction:

```
vrjd=18446744073709405858  vrjvtin=53378466540  vrjnow=53378320782
vrjpid=0  vrjstamp=0  vrjcharg=0
```

Three facts, and each kills a candidate:

- `d = 2^64 - 145758`. Not a huge elapsed time — an **unsigned underflow**.
- `vt_in - now = 53378466540 - 53378320782 = 145758`, **exactly** the same number.
  So `d` is `-(vt_in - now)` wrapped: at subtraction time `now` was BELOW `vt_in`.
- `vrjstamp == vrjcharg == 0` — the same cpu stamped and charged. **§8.4's
  unsynced-cross-cpu-TSC hypothesis is REFUTED**, by the field added to test it.

`vrjpid=0` is the idle thread: the most frequently re-dispatched thread there is,
and therefore the one most exposed to the window below.

### 9.2 Why the guard did not stop it

```c
uint64_t d = (now > t->vt_in) ? (now - t->vt_in) : 0;
```

reads `t->vt_in` **twice**, and `vt_in` is plain memory. `sched_charge_elapsed`
runs from `yield()` with interrupts enabled, so between the two reads a timer
tick can re-enter the scheduler, re-dispatch **this same thread**, and stamp a
NEWER `vt_in` in `rq_pop` (`sched.c:560`). The comparison then passes against the
OLD value while the subtraction uses the NEW one, and `now - vt_in_new`
underflows.

The guard is not wrong. It is simply not atomic with the operation it guards.

### 9.3 Proven in the emitted code, not just argued

Whether the compiler actually emits two loads is the crux — if it cached
`vt_in` in a register the race could not exist and this whole story would be
wrong. It does not cache it. Counting memory references to `vt_in` (struct
offset `0x27e8`) in `sched_charge_elapsed`:

| build | refs | which |
|---|---|---|
| double read (mutant `a548de06bdea1adc`) | **4** | `cmpq $0x0,0x27e8(%rax)` (null guard), `mov 0x27e8(%rax),%rax`, **`cmp 0x27e8(%rcx),%rax`**, **`sub 0x27e8(%rcx),%rax`** |
| single read (fixed `073a1e2a43eee51d`) | **2** | `cmpq $0x0,...` (null guard), `mov 0x27e8(%rax),%rax` |

The mutant's compare and subtract each re-read memory **independently**. That is
the race, present in the instruction stream. The fix collapses both to the one
register copy.

**This is why no execution-based mutation campaign was run to completion.** The
defect is ~1/6 per boot locally, so a 4-run mutant campaign (0/4 observed) has
P(miss) = 0.48 and proves nothing either way — it is NOT a refutation, only an
underpowered sample. The disassembly is a statement about the binary rather than
about a probability, and it settles the question that sampling could not.

### 9.4 The fix

Read once into a local:

```c
uint64_t in = t->vt_in;
uint64_t d  = (now > in) ? (now - in) : 0;
```

Deliberately NOT a clamp on `d`. Clamping would hide the underflow instead of
preventing it, and would leave the torn read in place to corrupt something else
later.

### 9.5 Chain from one torn read to permanent starvation

1. one charge lands with `d ~= 2^64`;
2. `dbg_vruntime += (d >> 10) * 1024 / w` adds ~`2^54`;
3. `g_dbg_floor` latches it — it is a monotonic maximum (`sched.c:257-258`);
4. every later thread is clamped to that floor on create/wake (`:138-142`);
5. the queue head then sits ~3.4e14 **above** the incumbents, `fair_candidate`
   picks the LOWER vruntime, and at ~8e7/500 ticks the gap needs ~2M ticks to
   close. `headpk` pinned at 1447 while `curpk` passes 1.7M.

One torn read, and the scheduler never picks that thread again.

### 9.6 Evidence, stated with its limits

- Fixed kernel `073a1e2a43eee51d`: **8/8** `smoke-evresize` clean, zero `vrjn`
  events. At a ~1/6 base rate that is ~77% power on its own — supporting, not
  conclusive, and it is §9.3 that carries the argument.
- Gates: `smoke-shell` 5/5, `smoke-evresize`, `smoke-rqfree`, `smoke-yieldstall`
  all PASS.
- The `vrjn` instrument stays in. On a fixed kernel an underflow is impossible,
  so any future `vrjn>0` is a real regression and names itself.

### 9.7 CI evidence (2026-08-24), and what it does NOT show

`e4c71e8` (the fix) is **fully green: 30/30 jobs across both suites** — ten
shards, both arch-bootstraps, aether-layer, code-graph, shard-check.

The pointed comparison is shard 3. On `1159c9d` (unfixed) it FAILED at
`smoke-nethammer`, and its heartbeat carried the inflation this DDR is about:

```
headvr=126100790075346602   headpk=1668754   preempt=10512
```

— i.e. ~1.26e17 against a healthy ~1e6, independent of the local reproduction
and on a different gate. On `e4c71e8` the same shard reports:

```
[nethammer] PASS — 2 distinct pids, 40,000 connect/close pairs, conn_err=0
shard 3: ALL PASS — 20 gates
```

**What is NOT shown, and cannot be from a green run:** `headvr` itself.
`boot_test.sh` echoes the serial log only on FAILURE, so a passing job never
prints a heartbeat — the healthy value is structurally unobservable in a green
CI log. So the CI evidence is "the gate that failed while exhibiting the
inflation now passes", which is consistent with the fix and is NOT a direct
measurement of `headvr` returning to ~1e6.

Two further honesty notes:

- One green run of `smoke-nethammer` is not proof that the inflation caused its
  failure. That attribution was flagged unproven when the failure was first read
  and it stays unproven; the gate may pass for unrelated reasons.
- The direct CI measurement would need `headvr` asserted by a gate rather than
  observed in a log. That is a cheap future arm — assert `headvr < 1e12` at the
  heartbeat — and is the honest way to make this checkable in CI. Not added here
  because the disassembly in §9.3 already settles the mechanism, and a new gate
  arm days from a deadline needs its own mutation check.


---

## 9.8 `[vrinflate]` — making the fix observable in CI

§9.7's gap: a GREEN ci log never prints a heartbeat (`boot_test.sh` echoes the
serial only on failure), so `headvr` returning to health was verifiable locally
and by disassembly but **not observable in CI at all**. This closes it, in the
`[apfreeze]` shape rather than as a gate arm: the heartbeat emits
`[vrinflate] curvr=… headvr=…` when either exceeds `1e12`, and `[vrinflate]` is
in `GLOBAL_FORBIDDEN`, so any gate whose boot hits it goes red and names itself.

**Why this one is entitled to a GLOBAL_FORBIDDEN slot when `[yieldstall]` was
not** (§8.2): a 5 s yield-spin was never shown to be fatal, so forbidding it
invented failures. Inflated vruntime is different — after §9.4 the underflow
that produced it is *impossible*, so an occurrence is a genuine regression. That
is exactly the bar §8.2 set for re-adding a sentinel, applied to a different one.

Threshold headroom, measured not guessed: vruntime is ~cycles/1024, so a 240 s
gate at 3 GHz reaches ~7.0e8, and a healthy boot at t=500 measured
**`headvr=238174`**. `1e12` is ~1400x the gate ceiling and ~4e6x the observed
value.

### Verification, decomposed

The two questions were tangled and are separated here:

- **A — does the detector work?** Deterministic, and it PASSES. With the
  threshold temporarily lowered to 1000, a healthy boot (`headvr=238174`) fires
  the sentinel and `boot_test.sh` correctly reddens the gate:
  `[smoke] FAIL — a probe reported '[vrinflate]' during this gate's boot.`
  Print -> GLOBAL_FORBIDDEN scan -> red, end to end. Threshold restored
  afterwards; kernel hash back to `de52208dd262cd2d`.
- **B — does the defect still occur on an unfixed kernel?** Inherently
  probabilistic and **NOT established by campaign**: the double-read mutant went
  0/9 across two campaigns. At a ~1/6 base rate P(0 in 9) = 0.19, so that is an
  underpowered sample, **not** a refutation, and it is recorded as such rather
  than dressed up. §9.3's disassembly is what settles the mechanism.

### Known limitation, stated up front

The sentinel can only fire on a boot that lives long enough to emit a heartbeat
(500 ticks). Short passing gates print none, so they are not covered. That is
acceptable — every observation of the inflation so far came from a long boot
(t=11500 locally, t=23500 in CI shard 3) — but it means a green short gate is
not evidence of absence.


### 9.9 The sentinel rode a green suite (2026-08-24)

`31535f2` — the commit that adds `[vrinflate]` to `GLOBAL_FORBIDDEN` — completed
**green**. The sentinel stayed silent across a full suite: no false positives,
no shard reddened by it.

That was the specific risk worth watching, and it is worth naming why. Earlier
the same day I added `[yieldstall]` to the same list on a signal never shown to
be fatal, and it reddened shards 2, 5, 8 and 9 — inventing failures rather than
detecting them, and costing a cycle to diagnose and revert. The difference is
not luck: `[yieldstall]` fired on behaviour that is merely *slow*, while
`[vrinflate]` fires only on a value that the §9.4 fix makes **impossible**, with
a bound measured at ~4e6x the observed healthy value rather than guessed.

Branch state: green on four consecutive commits — `364286e`, `e4c71e8`,
`8d765e6`, `31535f2`. Note §INV.15 still applies for promotion: three greens
must be on the SAME tip sha, so a consistently-green branch is not itself a
promotion credential.


---

## 9.10 DIRECT CI confirmation — and §9.7 was too pessimistic

§9.7 said `headvr` "is structurally unobservable in a green CI log". That is
true and it is also incomplete: `boot_test.sh` dumps the serial on **FAILURE**,
so a RED run shows it. Run 32733111805, shard 3, `67f25a9` (a docs-only commit,
so the kernel is `e4c71e8`'s fixed one) failed `smoke-nethammer` and printed
five consecutive heartbeats:

```
headvr=417450358  headpk=1624719  vrjn=0
headvr=427855053  headpk=2771338  vrjn=0
headvr=437703711  headpk=2819800  vrjn=0
headvr=447562732  headpk=2978529  vrjn=0
headvr=457428013  headpk=1763078  vrjn=0
```

**The fix is confirmed in CI by direct measurement**, not by inference:

| quantity | unfixed (`1159c9d`) | fixed (`67f25a9`) |
|---|---|---|
| `headvr` | 126,100,790,075,346,602 (~1.26e17) | ~4.2e8 - 4.6e8 |
| `vrjn` | (instrument absent) | **0** across every sample |
| `headpk` | 1,668,754 | 1.6M -> 2.9M, climbing |

4.2e8 sits just under §9.8's computed 240 s ceiling of ~7.0e8, which is the
value that section predicted from `cycles/1024` — the arithmetic and the
measurement agree. `headvr` advances ~10M per 500 ticks: normal accumulation,
not a jump. No starvation: the head is picked throughout.

## 9.11 …and `smoke-nethammer` STILL failed. The inflation was never its cause.

The same run failed. §9.7 flagged "one green nethammer run does not prove the
inflation caused its failure" and refused the attribution; this settles it in
the other direction — **the inflation is fixed and nethammer still times out**,
so the two are independent. Had that attribution been accepted when it was
convenient, this would now look like a regression instead of a separate defect.

`smoke-nethammer` is INTERMITTENT and **not** caused by anything in this DDR:
FAILED on `1159c9d`, PASSED on `e4c71e8`, FAILED on `67f25a9` — and every commit
since `e4c71e8` is docs-only or an above-1e12 print that healthy boots never
reach, so the kernel is unchanged across that pass and both failures.

**Four data points, one kernel** (updated after `44074f3` went green on BOTH
suites):

| commit | `smoke-nethammer` |
|---|---|
| `1159c9d` | FAILED |
| `e4c71e8` | PASSED |
| `67f25a9` | FAILED |
| `44074f3` | PASSED (both suites) |

Everything from `e4c71e8` onward is docs-only or an above-1e12 print that a
healthy boot never reaches, so the KERNEL IS BYTE-IDENTICAL across that
pass/fail/pass/pass. Two conclusions, both now firm:

1. **Not caused by any change in this branch.** An identical binary cannot
   alternate; the variable is the environment.
2. **The "budget too tight under CI load" hypothesis is strengthened, not
   proven.** Alternating pass/fail on one binary is exactly what a 240 s ceiling
   on a heavy probe produces when runner speed varies — but it is equally what
   any load-sensitive race produces, so this ranks the hypothesis, it does not
   confirm it. Measure the actual completion time across runs before touching
   either the budget or the code.

Recorded, NOT diagnosed (§NON-NEGOTIABLE 3). What the capture shows, for whoever
picks it up:

- it runs the full 240 s budget to t=23500 and does not finish; the probe is
  2 x 20,000 connect/close pairs at `QEMU_SMP=4`, so "the budget is simply too
  tight under CI load" is a live hypothesis and must be tested before any code
  change is considered;
- `max=2509629` on `cpu=2` with `bails=0` — one `switch_wait_offcpu` spin of
  2.5M iterations that did not bail. That is the **fifth unbounded spin**
  DDR-994 §8.3 flagged as missing from its inventory of four. Suggestive, not a
  diagnosis: `calls=101693` against `spins=2682874` averages 26 spins/call, so
  the 2.5M is one outlier and could as easily be a symptom of the stall as its
  cause.
