# DDR-989 — `smoke-evresize` / `smoke-agentpanel`: vruntime is sampled, so a sub-tick yielder is never charged

**Status:** root-cause HYPOTHESIS with a named mechanism. **Not implemented.**
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
