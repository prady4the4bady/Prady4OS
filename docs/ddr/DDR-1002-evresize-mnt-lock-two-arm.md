# DDR-1002 — §8.2 tested in BOTH directions: a two-arm `smoke-evresize` campaign

**Status:** PRECOMMIT — thresholds and stopping rules fixed BEFORE any run.
**Supersedes nothing. Extends:** DDR-989 (the fix), DDR-994 (the detector),
DDR-1000 §8 (the prediction), DDR-990 §4 (the both-directions standard).

---

## 1. What this measures, and — first — what it does NOT

This tests **DDR-1000 §8.2** and nothing else:

> Inflated `vruntime` starves whoever holds `mnt_lock`; a waiter then spins in
> an unbounded `yield()` loop forever. If that is right, **DDR-989's fix removed
> the trigger** and organic `mnt_lock` stalls should no longer occur.

**It does not bear on OPEN-1 route 1.** DDR-1000 §8.1 already settled that and
the point is easy to lose, so it is restated here as the first line of the file:

> Route 1 is `smoke-surfdestroy` hanging inside `sys_read`; this is
> `smoke-evresize` failing a resize round-trip with the guest still alive. Same
> lock, different gate, different symptom.

Route 1 is additionally **CI-only** and this campaign is **local**, which is the
second, independent reason a clean result here cannot close it — the same reason
DDR-1000 §9.3 refused to let 60 clean `smoke-surfdestroy` logs close it.

A session note written before this file framed the evresize campaign as "the
OPEN-1 route 1 test". That framing was wrong and is corrected here.

## 2. Why one arm is not enough — the flaw in the campaign already running

At the time of writing, `smoke-evresize` stands at 6/60 clean on kernel
`60b35c96d70253f5`, zero `[yieldstall]` lines of any kind.

A clean sweep of arm A alone is **the same under-powered claim DDR-1000 §3
rejected for route 2** — worse, in fact, because route 2 at least had a
*measured* base rate (1/20, DDR-985) to compute power against. The organic
evresize stall has **no measured rate at all**: it is known from two captures
(`build/gatelogs/lf-smoke-evresize.out` pid=45,
`build/gatelogs/vrj-6.out` pid=43), neither of which establishes a denominator.

So N=60 clean on the fixed kernel would license exactly one sentence — "it did
not happen in 60 boots" — with no power figure attachable to it. §NON-NEGOTIABLE
17 wants a denominator and there is none.

The fix is not more runs. It is a second arm.

## 3. The mutation withdraws exactly one property

DDR-989's fix is one load:

```c
    uint64_t in  = t->vt_in;                      /* fixed:  ONE read      */
    uint64_t d   = (now > in) ? (now - in) : 0;
```

The mutant restores the torn read and changes nothing else:

```c
    uint64_t d = (now > t->vt_in) ? (now - t->vt_in) : 0;   /* MUTANT: two reads */
```

This satisfies the DDR-990 §4 standard on every count:

- **One property withdrawn.** Single-read atomicity of `vt_in`. The `g_dbg_floor`
  clamp, `rq_pop`'s stamping, the runqueue, `mnt_lock`, and the `[yieldstall]`
  detector are all untouched.
- **The mutant self-identifies.** `g_vrjump_n` / `g_vrjump_d` (DDR-989 §8.4) stay
  compiled in, so a mutant boot that actually tears prints `vrjn>0` in its
  heartbeat. A mutant run with `vrjn=0` did **not** exercise the defect and is
  recorded as such rather than counted as a clean observation.
- **Distinct kernel hashes are mandatory.** Arm A is `60b35c96d70253f5`. If the
  mutant build hashes identically, the build did not take and the run is void
  (R1). That check has already earned itself once this project — DDR-990 §9's
  first mutant build failed while `kernel.bin` still held the fixed hash.

## 4. Arm B — measure the rate (N_B = 20)

Kernel: mutant. Gate: `smoke-evresize`. Ledger: separate from arm A's.

**Counting rule, fixed now.** A boot counts as a hit iff
`tools/ci/yieldstall_scan.py` reports an **organic unresolved** stall for it:
a `[yieldstall] site=mnt_lock` line with `pid != 0` and no `RESOLVED` partner.
`pid=0` is `smoke-yieldstall`'s own synthetic arm (DDR-994) and never counts.
Gate PASS/FAIL is **not** the measurement — the pre-fix captures failed the gate,
but the stall is the observable, and a mutant boot could stall without failing.

`k_B` = hits out of 20.

## 5. Precommitted decision rules — written before the data exists

**If `k_B = 0`:** the mutation does not reproduce the trigger locally at N=20.
§8.2 is then **UNTESTABLE BY THIS DESIGN**, and that is the finding. It is
recorded as a null and arm A's clean runs are **not** converted into a power
claim. This DDR precommits to reporting that outcome rather than re-tuning the
mutant until something fires — a mutant adjusted after seeing a null result is
no longer a test of the hypothesis that motivated it.

The leading explanation for a null is worth naming in advance so it is not
invented afterwards: the mutant is **today's kernel minus DDR-989**, not the
historical kernel the two captures came from. That tree also lacked DDR-981's
interrupt window and DDR-987's `g_net_lock`, among others. If the organic stall
needed one of those absences as well, reverting DDR-989 alone will not reproduce
it — and that would mean §8.2 names a *composite* trigger, of which DDR-989 is
only one term. A null is therefore informative about the design, not only about
the rate.

**If `k_B >= 1`:** let `r_lo` be the Clopper–Pearson 95% one-sided lower bound on
`k_B/20`. Then

```
N_A = ceil( ln(0.05) / ln(1 - r_lo) )
```

is the number of clean arm-A boots needed for 95% power against the *conservative*
end of the measured rate. Arm A runs to `N_A`, on ONE kernel hash.

**Cap.** If `N_A > 60`, the campaign stops at 60 and reports the power actually
achieved rather than the power desired. The deadline is 2026-08-31; an honest
partial figure beats an unfinished campaign.

**Refutation.** Any organic unresolved stall in arm A **refutes §8.2**: the
`mnt_lock` wait would then be independently unbounded, and bounding it becomes a
fix with a named mechanism behind it (which §NON-NEGOTIABLE 3 currently forbids,
precisely because no such artefact exists yet).

## 6. Arm A's existing 6 runs — the condition for keeping them

Arm A's 6 completed runs are on `60b35c96d70253f5`. Building the mutant and
restoring afterwards means arm A's kernel is rebuilt. Those 6 runs count toward
`N_A` **iff the restored build hashes to `60b35c96d70253f5` exactly**. If it does
not, arm A restarts from zero. A campaign spanning two kernels is not a campaign
(R1), and this session has already discarded four runs to that exact mistake.

## 7. Cost

Arm B is 20 runs at ~2 min = ~40 min. If `r` is high the derived `N_A` is small
and the two-arm design is *cheaper* than the 60-run single arm it replaces, as
well as strictly stronger. If `r` is low, `N_A` is capped at 60 and the cost
equals the original plan plus arm B.

---

## 8. Arm B is faithful — proven from the shipped binary, not from the source

`vrjn=0` on the mutant in its first 6 boots raises the one question that would
void arm B entirely: **`t->vt_in` is plain `uint64_t`, not `volatile`, so the
compiler is free to common-subexpression the two loads back into one.** If clang
had done that, the mutant would be the *fixed* kernel wearing a different hash —
the hash differs anyway, because the instrument line changed — and arm B would be
measuring nothing. A distinct hash proves the build changed; it does not prove
the semantics did.

Checked rather than assumed, in `build/kernel.elf` at `sched_charge_elapsed`
(`ffffffff80012370`):

```asm
ffffffff800123af:  48 3b 81 e8 27 00 00   cmp  0x27e8(%rcx),%rax   ; read 1: now > t->vt_in
ffffffff800123c4:  48 2b 81 e8 27 00 00   sub  0x27e8(%rcx),%rax   ; read 2: now - t->vt_in
```

Two independent loads of the same offset, fifteen instructions apart, with a
conditional branch between them. **The mutation is live in the binary.** Arm B is
a real arm.

### 8.1 What `vrjn=0` therefore means

Not "the mutation failed". It means the torn read is *reachable but did not
occur*: the window is a timer tick landing between those two loads. That the
window is narrow was always implied by DDR-989's evidence — the captured tear was
on **pid 0**, the idle thread, "the most re-dispatched thread there is" — but the
disassembly is what turns `vrjn=0` from an ambiguous reading into an interpretable
one.

The comparison that makes it interpretable is that both arms carry the same live
instrument and it reads zero on the fixed kernel too (arm A run 1: `vrjn=0` in all
23 heartbeats). So a null in arm B separates cleanly into a statement about the
*rate of the tear*, not about whether the instrument or the mutation works.

### 8.2 N_B stays at 20

The precommit in §4 fixed `N_B = 20` and §5 fixed what a null means. Six zero
boots is not a reason to extend N — extending it *because* the early runs came
back clean is the same post-hoc adjustment §5 forbids for the mutant, applied to
the sample size instead. The campaign runs to 20 and reports what 20 shows.

---

## 9. RESULT — the mutation works, the experiment does not. §8.2 stays UNTESTED.

Arm B: **20/20, one kernel hash `42459dce865c71c6`**, ledger
`build/gatelogs/campaign/smoke-evresize.mut.ledger.txt`.

| measure | result |
|---|---|
| **`k_B` — organic unresolved `mnt_lock` stalls** (§4's precommitted rule) | **0 / 20** |
| tear actually fired (`vrjn > 0`) | **4 / 20** |
| tear fired *within the gate's assertion window* | **1 / 20** |
| arm B gate outcome | 20 PASS, 0 FAIL |

### 9.1 The mutation is real — three independent confirmations

1. **Two loads in the shipped binary** (§8): `cmp 0x27e8(%rcx),%rax` then
   `sub 0x27e8(%rcx),%rax`. No CSE.
2. **The defect fired, with DDR-989's exact signature.** Run 7:
   `vrjn=2 vrjd=18446744073709453330 vrjvtin=159456282742 vrjnow=159456184456
   vrjpid=0 vrjstamp=0 vrjcharg=0` — `vtin - now = 98286`, `d = 2^64 - 98286`,
   on **pid 0** (the idle thread), stamping and charging cpu **both 0**. That is
   DDR-989 §9's capture reproduced on demand, down to the same-cpu detail that
   refuted the unsynced-TSC story.
3. **The fixed kernel reports zero on the same live instrument** (arm A run 1:
   `vrjn=0` in all 23 heartbeats), so 4/20 vs 0/20 is a contrast between arms,
   not an artefact of the counter.

**The tear rate on the reverted kernel is 4/20 = 20%.** That is the denominator
§NON-NEGOTIABLE 17 asks for, and it is the number this campaign actually earned.

### 9.2 Why `k_B = 0` is a verdict on the DESIGN, not on §8.2

Of the 4 boots that tore, **3 tore after the gate had already finished
asserting** and 1 was essentially coincident with the last sentinel:

| run | `vrjn` | first tear reported (line) | last gate sentinel (line) | in window? |
|---|---|---|---|---|
| 7  | 2 | 450 | 435 | no |
| 16 | 1 | 453 | 435 | no |
| 18 | 1 | 440 | 441 | **yes** |
| 20 | 1 | 451 | 435 | no |

Run 7 is the clearest: the tear is reported at heartbeat `t=8000`, the previous
heartbeat `t=7500` is at line 449, and `PRADYOS_EV_RESIZE_OK` /
`PRADYOS_CLOSE_OK` are at lines 434–435 — **before** the `t=7500` heartbeat. The
gate had finished its work while the boot ran on.

**Caveat, stated because it bounds the claim rather than sharpening it:** `vrjn`
is latched and printed by the heartbeat, so a tear reported at `t=N` occurred
somewhere in `(N-500, N]`, not at `N`. The lines above therefore bound the tear
time; they do not pinpoint it. For runs 7, 16 and 20 the bound is still entirely
after the sentinels, so the conclusion survives the caveat. For run 18 it is
inside.

So the effective sample size for testing §8.2 is not 20. **It is about 1.** A
campaign with N≈1 has no power, and `k_B = 0` is what an experiment with no power
returns whether or not the hypothesis is true.

### 9.3 The decision, per §5's precommitted rule

`k_B = 0`, so §5 fires as written:

> §8.2 is **UNTESTABLE BY THIS DESIGN**, and that is the finding. It is recorded
> as a null and arm A's clean runs are **not** converted into a power claim.

That rule was written before any run and it is honoured here rather than
reinterpreted. §8.2 — "DDR-989's fix removed the trigger for the organic
`mnt_lock` stall" — remains **neither supported nor refuted**.

### 9.4 Arm A is STOPPED at 8, deliberately

Arm A's kernel was rebuilt after the mutation and hashes to
**`60b35c96d70253f5`**, identical to its first 8 runs — the build is
reproducible and those runs were valid (§6's condition met). They are still not
convertible into a power claim, because §5 says a null in arm B removes the
denominator that would license one.

Running arm A to 60 would therefore consume ~1.7 hours of QEMU to produce a
sentence this file has already refused to write. **It is stopped at 8** and the
host is returned to release work. Stopping is the conclusion, not an abandonment.

### 9.5 What a design with power would need — NOT run here

The blocker is placement, not rate: the tear must fire while the gate is still
asserting. Any future attempt must precommit, separately and before running:

- a way to make the tear fire inside the window (the tears cluster late, on the
  idle thread, once `ymask` is in the millions — so the window and the load that
  produces tears are currently disjoint), **and**
- an in-window verification that is checked *before* the stall count is read, so
  a repeat of this null is distinguishable from a real absence at the time it
  happens rather than afterwards.

Recording it as unbuilt, per DDR-990 §12's precedent: naming the instrument that
would settle a question and stating plainly that it does not exist is worth more
than a campaign that cannot settle it.
