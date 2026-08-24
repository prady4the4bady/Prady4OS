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
