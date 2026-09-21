# DDR-1131 — What each hunt outcome will mean, registered BEFORE the data

**Status:** pre-registration. **Docs-only, no code change, no gate, `kernel.bin`
NOT rebuilt.** NO FIX, NO MECHANISM NAMED, **OPEN-2 DOES NOT CLOSE**, no open
issue moves (OPEN-1/2/12/13 untouched).

## 0. Why this exists at all

800 boots are in flight (DDR-1130 §2: runs `35581509323` and `35582316759`,
`lanes=20 runs=20 hunt=32`, both pinned). When they land there will be a strong
pull to read whatever arrives as confirming whichever mechanism is nearest to
hand. This project has already paid for that failure mode twice — DDR-1128 §3's
*"field-for-field shape DDR-981 recorded"* reading **pointed the wrong way**, and
DDR-1042 is the record of a plausible number with nothing under it.

So the readings are fixed **now, while the outcome is unknown**. This is the
discipline DDR-1000 §3 used when it set route 2's threshold before running E1,
and DDR-1002 used when it declared its arms before spending 20 runs.

**Nothing here is a claim about OPEN-2.** It is a claim about what will and will
not follow from each possible capture.

## 1. The state of the question

DDR-1118 proposed a **double resume**: a thread switched in twice with no save
between. It shipped the two fields that would settle it and stated the
discriminator in advance — `disp == saves + 2`, with `rq_on = 1` as the named
precondition. DDR-1120's fire then answered **against** it: `rq_on=0`,
`disp=40669 saves=40668`, the healthy identity, on three exact witnesses. That is
the instrument working, not failing.

But DDR-1120 §5(b) also recorded what the identity **cannot** see, and it is the
live candidate:

> `kmalloc` does not zero (NON-NEGOTIABLE 10), so a **reissued TCB** carries its
> previous owner's counters; a parked thread satisfies `old_disp == old_saves`,
> and one `dispatches++` under the claim yields `disp = saves + 1` — **the healthy
> identity on a recycled object.**

## 2. One candidate closed here, by reading, not by data

DDR-1119 named `switch_wait_offcpu_sched`'s bounded handshake as where a third
fire would point *or away from*. Read in full (`sched.c:700-724`, call site
`:1555`): on hitting `SWITCH_WAIT_MAX_SPINS` the caller **declines the pick** —

```c
if (!switch_wait_offcpu_sched(next, cpu)) {
    if (!next->is_idle) rq_push(cpu, next);   /* give it back */
    next = g_idle[cpu];                       /* run idle instead */
    ...
}
next->dispatches++;                           /* increments IDLE, not next */
```

It never switches into the contended thread, so **the bail path cannot produce a
double resume**, and `dispatches++` past it belongs to idle. A fifth candidate
removed, on the same footing as DDR-1119's four. **No defect is alleged in that
function** — it is correct for what it was built to do.

## 3. THE PRE-REGISTERED READINGS

`[schedcheck]` and `[ringwalk]` are **both** in the campaign's `SIGNALS`
(`open2_hunt_campaign.sh:101`), `[ringwalk] RECYCLED site=sched_tick` ships under
`#if OPEN2_HUNT` (`sched.c:2005`), and the hunt builds at `OPEN2_HUNT=32` — so
both can appear **in one capture**, and their co-occurrence is readable.

| capture | reading, fixed in advance |
|---|---|
| `[schedcheck]` with **`disp == saves + 2`** | **Double resume confirmed.** DDR-1118's mechanism, named. `rq_on=1` alongside corroborates via its stated precondition. |
| `[schedcheck]` with **`rq_on=1`** but `disp == saves + 1` | Queued-while-resumed **without** a second dispatch — DDR-1118's precondition **without** its consequence. A **narrowing, not a mechanism.** |
| **`[schedcheck]` AND `[ringwalk] RECYCLED` in one capture** | **The recycled-TCB reading (DDR-1096 §3) is named**, and it explains why §1's identity looked healthy: the counters belong to the previous owner. **This is the outcome that would move OPEN-2.** |
| `[schedcheck]` with `rq_on=0`, `disp == saves + 1`, **no `[ringwalk]`** | **DDR-1120 reproduced, nothing new.** Consumed frame; double resume refuted on that artefact; recycling **not** evidenced. Record it and say so. |
| `rq_on=?` | **The field itself is untrustworthy** (DDR-1118 §8.1's single-character guard). Read nothing else from that line. |
| `[apfreeze]` only, no `[schedcheck]` | Resolve the RIP **against its own binary** (§INV.18) before matching on the sentinel — `[apfreeze]` has **≥5 producers**, and `sched.c:1857`'s halt loop is one (DDR-1129). |
| **no signal in 800 boots** | A bound and **nothing else.** See §4. |

## 4. What a null will and will not license

0 in 800, pooled with DDR-1127's 0/60 on a different binary — **which must not be
done**; that DDR's own NOT CLAIMED forbids it. On **this** binary the prior is
DDR-1128's 1 in 60.

0 in 800 gives a 95% upper bound of **0.37%** per boot. That is a real bound and
it is **not** a refutation of DDR-1128's occurrence: a single observed event at
1/60 and a null at 800 are in tension but not contradiction, and the honest
statement would be that the rate is **lower than one occurrence in sixty
suggested**, not that the occurrence did not happen.

**A null names no mechanism and closes nothing.** DDR-1129 §7 item 1 stays owed
either way.

## 5. NOT CLAIMED

* **No mechanism is named and OPEN-2 does not move.** Every row of §3 is a
  conditional about a capture that does not yet exist.
* **No fix is proposed**, and NON-NEGOTIABLE 3 is not disturbed — §3's first and
  third rows describe what *would* constitute a named mechanism, which is not the
  same as having one.
* **No defect is alleged** in `switch_wait_offcpu_sched`, `rq_push`, `rq_take` or
  the `on_cpu` handshake; §2 removes a candidate, it does not accuse the code.
* **DDR-1118 is not withdrawn.** Its mechanism is refuted *on DDR-1120's
  artefact* and its instrument is what produced that answer.
* **No rate is claimed.** DDR-1128's 1-in-60 is one occurrence, and §4 treats it
  as a prior, not a measurement.
* No code change, no gate, no new sentinel; `GLOBAL_FORBIDDEN` 77, 179 gates,
  `kernel.bin` not rebuilt, so the size/headroom pair and `ci-docstate-check` are
  unaffected.
