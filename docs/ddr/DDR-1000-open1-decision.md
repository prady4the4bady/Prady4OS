# DDR-1000 — OPEN-1: the decision, and the evidence still missing

**Status:** DECISION. Requested by the operator directive of 2026-08-29 §3,
which asked for OPEN-1 to be closed with reasoning recorded, **or** for the
missing evidence to be named exactly. This takes the second option, names it,
and then goes and gets it (§5) rather than leaving it as a deferral.

**Decision: OPEN-1 does NOT close today.** Two of its three routes are
unresolved, and the reason is stated as a number, not a feeling.

---

## 1. What the directive got right, and the one correction

The directive states the honest gap precisely: the DDR-990 hammer's crash
signature is a `#GP`, OPEN-1's original artefact was a `#PF`, and DDR-985 did
not establish they are the same defect. That is correct.

The one correction is that it is **three** routes, not two — DDR-990 §12 already
recorded this, and it is the reason the "is it fixed?" question keeps resisting
a clean answer:

| # | route | artefact | status |
|---|---|---|---|
| 1 | **hang**, no panic — stops after `SYSFSTAT OK`, inside `sys_read` → `vfs_read` → SFS | *no exception at all* | **OPEN** |
| 2 | ring-0 **`#PF`**, after `[sfs] 64K write/read byte-exact OK` | `#PF` | **OPEN** (under-powered) |
| 3 | ring-0 **`#GP`**, `0xDDDDDDDDDDDDDDDD` in `tcp_new_port` | `#GP` | **CLOSED** (DDR-990 §9) |

"OPEN-1" names a *gate symptom* — `smoke-surfdestroy` missing
`PRADYOS_SURFDESTROY_CHURN_OK` — and three mechanisms can produce it.

## 2. Route 3 is genuinely closed, and it is the strongest result in the file

DDR-990 §9 proved the lwIP `g_net_lock` fix in both directions on demand rather
than by sampling: 40,000 connect/close pairs clean on the fixed kernel; `#GP` at
`tcp_new_port+0x2d` with `RDI=0xDDDDDDDDDDDDDDDD` in under 1,000 iterations on
the reverted one. Mutation-checked both ways, distinct kernel hashes.

That is a real fix and it should be credited as one. It is also, as DDR-990 §12
put it, "the one it closes is the one that was never OPEN-1's own artefact."

## 3. Route 2 is not closed because 20/20 is not enough — the arithmetic

The measured base rate is **1/20** (DDR-985, kernel `d31b4023b0f74d06`). A clean
20-run campaign on an *untouched* defect at that rate has probability
`0.95²⁰ = 0.358`. So:

> **A clean 20/20 happens roughly one time in three even if nothing was fixed.**

Power is therefore ~64%, and "did not reproduce in 20 runs" is evidence, not
proof. This is the same trap §NON-NEGOTIABLE 2's 20× rule exists to avoid, and
it bites harder here because the base rate is low.

To reach 95% power the campaign needs **N = 59** (`0.95⁵⁹ = 0.048`). Rounded to
**N = 60**.

## 4. Route 1 cannot be closed by any campaign, because it prints nothing

Route 1 is a hang. No panic, no exception, no register dump — the log simply
stops between `SYSFSTAT OK` and `SYSREAD OK`. A campaign can only ever report
"it did not happen again", which is exactly the under-powered claim of §3 with
no artefact even in principle.

DDR-990 §12 named the instrument that would settle it and recorded that it does
not exist:

> "What would settle route 1 is a watchdog on the `SYSFSTAT OK` → `SYSREAD OK`
> transition, which does not exist. That is the honest next instrument, and it
> is not built."

DDR-994 built a *partial* one: `[yieldstall]`, which reports when a `yield()`
spin exceeds a bound. `mnt_lock` (`vfs/vfs.c:27`) is an unbounded
`while (exchange(&m->busy,1)) yield();` sitting **directly on the `vfs_read`
path** where the captures hang, so it is the leading candidate. But DDR-994 was
explicit that it is a detector, not a fix, and not a claim that `mnt_lock` *is*
OPEN-1 — if the next occurrence prints no `[yieldstall]` line, the hypothesis is
refuted, which is itself a result.

**Nothing has read that detector's output yet.** A green suite does not answer
it: `[yieldstall]` is deliberately *not* in `GLOBAL_FORBIDDEN` (removing it was
a correction recorded at DDR-994 — it had reddened four shards on a signal never
shown to be fatal), so a RESOLVED stall sits silently inside a **passing** log.

## 5. The exact evidence still needed — and it is being gathered, not deferred

**E1 — route 2, statistical.** `smoke-surfdestroy` × **60** on the current
kernel, zero ring-0 `#PF`. That is 95% power against the measured 1/20 rate.
*Started while writing this*, kernel `a9cd9ed1114994b8`, via
`tools/ci/campaign.sh start smoke-surfdestroy 60`.

**E2 — route 1, instrumental.** Two parts, in order:
1. Grep CI job logs for `[yieldstall]` — specifically
   `RESOLVED site=mnt_lock`. Cheap, and it tests DDR-994's hypothesis against
   data that may already exist. Either outcome is informative: lines present
   means `mnt_lock` genuinely contends on this path; none present across many
   suites weakens it.
2. If (1) is silent, build the transition watchdog DDR-990 §12 named — a bounded
   timer on `SYSFSTAT OK` → `SYSREAD OK` that prints *where* it was when the
   deadline passed. A hang that prints nothing cannot be diagnosed; the whole
   job of this instrument is to convert it into one that prints something.

## 6. What would make OPEN-1 closeable, stated as a checklist

- [ ] E1 returns 60/60 clean → route 2 closed at 95% power.
- [ ] E2 returns either a named mechanism for route 1 (then fix it, gate it,
      mutation-check it) **or** a positive refutation of the `mnt_lock`
      hypothesis plus a watchdog that has run clean across a campaign.
- [x] Route 3 closed (DDR-990 §9).

## 7. If the deadline arrives first

Then OPEN-1 is recorded **OPEN** in the release notes with this file as its
statement, and `v1.0.0` ships with a known, documented, intermittent
`smoke-surfdestroy` failure at a measured ~1/20 on one gate — *or* it does not
ship, which is the operator's call and not mine to make silently.

What will **not** happen is OPEN-1 being marked closed on a green streak.
DDR-985 §Claim-A was written that way once, and §3's arithmetic is why it was
wrong.


---

## 8. E2 step 1 RESULT — `mnt_lock` does stall organically, and the captures are PRE-DDR-989

The grep was worth doing before writing code. `[yieldstall]` has fired outside
its own gate, on real ring-3 pids, in **two independent captures**:

```
build/gatelogs/lf-smoke-evresize.out :
  [yieldstall] site=mnt_lock spins=24589 ticks=500 pid=45 cpu=0
build/gatelogs/vrj-6.out :
  [yieldstall] site=mnt_lock spins=27998 ticks=501 pid=43 cpu=0
```

Four things about these, in the order they matter:

1. **They are not the detector's own arm.** `smoke-yieldstall`'s arm B holds
   `mnt_lock` deliberately and reports `pid=0`. These are `pid=45` and `pid=43`,
   and the adjacent heartbeats name `cur=AETHERD` at exactly those pids — the
   AETHER daemon is the thread spinning.
2. **Neither ever RESOLVED.** `OPENED: 1, RESOLVED: 0` in both files. The
   reporter prints a RESOLVED partner when the spin ends; there is none. So the
   wait did not end for the rest of the boot.
3. **Both boots FAILED**, both with `[evresize] FAIL — client did not honor the
   resize`, and in both the `preempt` counter freezes at the stall and never
   moves again (1606 → 1630, then flat; 1594 → 1618, then flat) while `curpid`
   keeps changing. That is the "frozen `preempt` with a live `curpid`" third
   signature recorded in the handoff and never explained.
4. **And the kernel in both had the DDR-989 defect LIVE.** `vrj-6.out` carries
   `vrjn=1 vrjd=18446744073709405858` — that is the DDR-989 §9 instrument
   reporting the poisoned charge — and both show `curvr ≈ 1.79e16` against
   `headvr = 1.80e16`. A run on today's fixed kernel shows `vrjn=0` and
   `curvr ≈ 1.9e7`: **nine orders of magnitude** apart. These captures predate
   the fix.

### 8.1 What this is, and what it is not

It **is** the first organic evidence that DDR-994's `mnt_lock` hypothesis names
a real contention: an unbounded `yield()` spin that, at least once, never ended.

It is **not** OPEN-1 route 1. Route 1 is `smoke-surfdestroy` hanging inside
`sys_read`; this is `smoke-evresize` failing a resize round-trip with the guest
still alive. Same lock, different gate, different symptom. Asserting they are one
defect on the strength of a shared lock name would be exactly the colour-matching
this file's §1 warns about, and which DDR-985's Claim A already did once.

### 8.2 The candidate explanation, and why it may already be gone

Inflated `vruntime` starves whoever holds `mnt_lock`; a waiter then spins in an
unbounded `yield()` loop forever. That composes the two known defects rather than
requiring a third: DDR-989 supplies the starvation, DDR-994 supplies the
unbounded wait. It also explains the frozen `preempt` — the runqueue stops making
progress — without needing a new mechanism.

If that is right, **DDR-989's fix removed the trigger** and organic `mnt_lock`
stalls should no longer occur. That is a prediction, and it is falsifiable.

### 8.3 The measurement is already running

E1's campaign (`smoke-surfdestroy` × 60, kernel `a9cd9ed1114994b8`) writes one
log per run under `build/gatelogs/campaign/`. Grepping those 60 logs for
`[yieldstall] site=mnt_lock` with no RESOLVED partner tests §8.2 **at no extra
cost** — the same campaign answers E1 and E2 step 2 at once.

- Zero unresolved organic stalls in 60 boots → §8.2 supported, and the route-1
  watchdog may not need building at all.
- Any unresolved organic stall on the fixed kernel → §8.2 is refuted, the
  `mnt_lock` wait is independently unbounded, and bounding it becomes the fix
  with a named mechanism behind it.

Either way this stops being a guess. **Do not write the watchdog until this
campaign reports** — DDR-990 §12 called the watchdog "the honest next
instrument", and it may turn out to be an instrument for a defect that no longer
exists.

---

## 9. E1 COMPLETE — 60/60, and the decision

Kernel **`5349db4d791cc2ab`**, ledger `build/gatelogs/campaign/smoke-surfdestroy.ledger.txt`:

```
campaign_chunk: smoke-surfdestroy now 60/60 — pass=60 fail=0 distinct_kernels=1
```

**60 runs, zero failures, exactly one kernel hash.** The single-hash check is not
ceremony: an earlier attempt at this campaign was contaminated when the tree was
edited under it, and the hash column is what makes that detectable instead of
silent (R1).

### 9.1 Route 2 — CLOSED at 95% power

Measured base rate 1/20 (DDR-985). `0.95⁶⁰ = 0.046`, so if the ring-0 `#PF` were
still present at its measured rate, a clean 60-run campaign would occur under 5%
of the time. That is the threshold §3 set **before** running, not one chosen
after seeing the result.

### 9.2 Route 3 — CLOSED (DDR-990 §9), unchanged

### 9.3 Route 1 — NOT closed, and the E2 scan does NOT bear on it

`yieldstall_scan.py` over all 60 logs:

```
scanned=60  with_yieldstall=0
organic_unresolved=0  synthetic_unresolved=0
```

The scanner's own verdict says "CONSISTENT, not proven". **It is weaker than
that, and saying so matters more than the clean result reads.**

The pre-fix organic `mnt_lock` stalls (§8) were captured in **`smoke-evresize`**
(shard 0) and a DDR-989 vruntime capture — **not** in `smoke-surfdestroy`
(shard 6). And `smoke-surfdestroy` emits **zero** `[yieldstall]` lines even in a
single log, i.e. the instrument never engages on that gate at all.

So 60 clean surfdestroy logs are not evidence about `mnt_lock`; they are 60
observations of a gate that does not exercise the path. Reading them as support
for §8.2 would be the same error as citing a passing test that never ran the
code — which is precisely what DDR-973 §6 and DDR-996 each caught once.

There is a second, independent reason route 1 stays open: **route 1 has only ever
been observed in CI, and this campaign is local.** 60 local runs bound the local
rate; they say little about a signature that has never reproduced off a CI runner.

What *is* true and worth recording: `smoke-surfdestroy` (shard 6) has been green
in every full-suite CI success since the DDR-981/987/989 fixes — `ca85e35`,
`b3573b9`, `2dbcbe8`, `0611a59`, `513ce6b`, `e1259ab`. No recurrence. But route
1's rate was never measured, so a run of greens cannot be converted into a power
figure the way §3 did for route 2.

### 9.4 The decision

**OPEN-1 closes for routes 2 and 3. Route 1 remains OPEN**, now with its scope
narrowed to exactly one thing: a **CI-only hang** in `sys_read`/`vfs_read` with
no artefact, no local reproduction, and no recurrence in six CI suites.

Two things would settle it, in cost order:

1. **An `smoke-evresize` campaign under `yieldstall_scan.py`.** That is the gate
   where organic stalls were actually captured, so it is the test §8.2 needs and
   this campaign was not. Cheap — the tooling already exists.
2. The `SYSFSTAT OK` → `SYSREAD OK` watchdog DDR-990 §12 named, if (1) comes
   back clean and route 1 still needs an instrument.

### 9.5 Release position

`v1.0.0` is no longer blocked on routes 2 or 3. Route 1 is a documented,
CI-only, non-reproducing intermittent with a named next test. Whether that ships
is the operator's call and is stated for them rather than decided here — but the
honest summary is that OPEN-1 is now **one narrow open question**, not the
three-signature tangle DDR-990 §12 found.

---

## 10. §10 — CORRECTION: the E2 scan in §9.3 was VACUOUS

§9.3 reported `scanned=60 with_yieldstall=0` and reasoned that the result did not
bear on route 1 because `smoke-surfdestroy` is the wrong gate. That reasoning was
right. It was also not the worst problem, and the worse one was mine.

**The scan read the wrong files.** `campaign_chunk.sh` keeps one file per run,
and that file is `make` **output**. The guest's serial capture goes somewhere
else entirely — for `smoke-evresize`, `build/evresize.log`, which every run
overwrites. Verified on a completed run:

```
lines: 45
serial markers ([smoke] prefix): 0
hb lines: 0
```

Zero. So the scan could not have found `[yieldstall]` even in a boot that emitted
it. "No organic unresolved stall across 60 logs" was a statement about nothing.

### 10.1 Why this matters more than the wasted scan

This is the same defect class this file keeps catching in *other people's* work
and in my own code — a check that can only ever return the answer you were hoping
for. DDR-996's first arm B, DDR-990's cross-CPU gate, DDR-997's clamp assertion:
each was caught before it was believed. This one I walked into.

**The conclusion in §9.3 survives**, because route 1 was not closed and the scan
was explicitly not used as evidence for closing it. Had it been load-bearing, this
would have been a false close — which is the entire failure mode §3 exists to
prevent, arriving by a different door.

### 10.2 Fix

`campaign_chunk.sh` takes an optional serial-log path and snapshots each run's
capture to `<gate>.<i>.serial.log` **before** the next run truncates it. The
evresize campaign restarts from zero with snapshots on; its first four runs are
**discarded, not counted**, because they have no retrievable serial evidence — a
run whose artefact cannot be read is not an observation.

### 10.3 What this does not change

Route 1 is still open, still CI-only, still without a local reproduction. The
evresize campaign is still the right next test — it is now merely a test that can
actually return an answer.
