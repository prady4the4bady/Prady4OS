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
