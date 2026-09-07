# DDR-1092 — THE rq-3 VERDICT IS COMPUTED FROM THE RACY SAMPLE THE SOUND ONE WAS BUILT TO REPLACE

Status: IMPLEMENTED (verdict change + validity guard), M1/M2/M3, no new gate
Date: 2026-09-07
Supersedes nothing. Corrects DDR-1064's verdict wiring and one sentence of the
DDR-1004 SKIP branch's own comment.

---

## 1. THE ARTEFACT

CI 34118029080, **shard 4, `smoke-smplock`**, tip `17e09ab` (DDR-1090). The
shard's own post-gate assertion printed `kernel.bin: OK` (DDR-1035), so the
binary is the one the `build` job produced.

```
[smp] resched FAIL ipis=0 ran=1 idle=1 idle2=1 kidle=0 kkick=0
```

preceded by forty lines of a healthy boot — FAT and SFS complete, `cross-wake
OK`, `sched cross-CPU OK`, `ap preempt OK` — and the gate then burned its whole
120 s window because `resched FAIL` is a `GLOBAL_FORBIDDEN` entry (DDR-791), so
it reddens **whichever gate happens to boot**, not the gate that owns the
assertion. `smoke-smplock` asserts per-CPU lock bring-up and has nothing to do
with rq-3.

**`kidle=0` is DDR-1074's SOUND EXONERATING reading**, in its own words:

> `kidle=0` -> no idle non-self CPU was visible to the kernel; no kick was owed
> and the FAIL is a sampling artefact, whatever `idle=`/`idle2=` happen to say.

So this is the third occurrence of the shape (DDR-1064: `e9ed2c9`,
`smoke-smppreempt` shard 4; DDR-1074: `0da019f`, `smoke-smp` shard 7; here),
and the **second of the two captures that carry the fields at all** — and
**both of those read `kidle=0`.** Two is not a rate and none is claimed.

### 1.1 NOT ATTRIBUTED TO DDR-1090, AND NOT EXONERATED EITHER

The tempting sentence is "the diff is elsewhere", and DDR-1042 forbids it. What
can honestly be said, each measured:

* The signature **predates** this tip by two captures, both on commits that
  changed **no build input** — so it is not introduced here.
* `git diff --name-only 17e09ab 5d05b7d` lists **six Markdown files and nothing
  else**, so `5d05b7d` runs the **same kernel binary**, and that tip went
  **2/2 green**. Counting the push suite on `17e09ab`, this binary has **three
  green suites and one red**.
* And against all that: DDR-1090's change is two loads and a branch inside
  `yield()`, which the settle loops of this very proof call — and the artefact
  is **a question of which CPU is idle at one instant**. Timing is precisely
  what it depends on. **So no exoneration is claimed.** What is claimed is
  narrower and does not need one: the printed reading is one a **correct**
  kernel produces, established by DDR-1074 independently of this commit.

---

## 2. THE FINDING

DDR-1064 built `kidle=`/`kkick=` for exactly one reason, stated in its own
comment at `sched.c:1882`:

> the rq-3 proof used to re-derive it from outside the call and **could not**: a
> CPU can leave idle before the call (DDR-1004's race) or enter idle after it
> returns (DDR-1030's own race) … DDR-1014 made the two loops ask the same
> QUESTION; this makes them ask it at the same INSTANT.

It then **printed the sound value in the FAIL branch and left the verdict
computed from the racy one it was built to replace.** Read the verdict as
shipped (`main.c:1077`):

```c
if (g_rp_ran && ipi_expected && !idle_seen && g_resched_ipis == before) → SKIP
else if (g_rp_ran && (!ipi_expected || g_resched_ipis > before))        → OK
else                                                                    → FAIL
```

`idle_seen` is the proof's own sample, taken **before** `sched_unblock`, and
DDR-1004's comment already calls the window "not zero". `dbg_ub_saw_idle` is the
kernel's answer at the instant the kick loop ran. The capture in §1 is what the
gap costs: `idle_seen=1` (the racy proxy) and `saw_idle=0` (the sound one)
disagree, the verdict consults the proxy, and a boot in which **no kick was ever
owed** is reported as a scheduler failure on an unrelated gate.

**An instrument that is printed but not consulted is a comment.** This is the
DDR-1046/1060/1074/1080/1089/1090 class — a control that cannot act in the case
it exists for — arriving here as a control that *can* see the case and is simply
not wired to the decision.

### 2.1 WHY THIS IS NOT THE CHANGE DDR-1074 REFUSED

DDR-1074 §6 considered narrowing the verdict and refused, and the refusal is
kept intact because it is about the **other direction**:

> the NARROWER change — FAIL only on the convicting reading — … is not built
> because **THERE IS NO SOUND CONVICTING READING TO GATE ON**.

That is true and unchanged: `kidle=1 kkick=0` is ambiguous between a
BSP-only-idle boot (correct) and a genuinely missed kick, and this DDR does not
touch it. **This change is the exonerating direction**, which DDR-1074's own
table certifies as **SOUND** rather than ambiguous, and which it never argued
against. One direction of a two-directional question was settled; this settles
the other one, in the direction that was already settled.

---

## 3. COVERAGE IS PRESERVED, AND THAT IS THE LOAD-BEARING CHECK

DDR-1030 §3 and DDR-1064 §6 each refused a SKIP once, and their stated objection
was to **collapsing the whole case to SKIP**, because a genuinely broken kick
also prints `ran=1` (the thread is picked up a timer tick later). That objection
does not reach this change, and the reason is mechanical rather than rhetorical:

| kernel state | `kidle` | `kkick` | verdict before | verdict after |
|---|---|---|---|---|
| no idle non-self CPU at the kick instant — no kick owed | 0 | 0 | **FAIL** | **SKIP** |
| DDR-1014's defect (break on the CALL, idle AP skipped) | 1 | 0 | FAIL | **FAIL** |
| kick delivered, counter wrong | 1 | 1 | FAIL | **FAIL** |
| kick delivered and counted | 1 | 1 | OK | OK |

**Only the first row moves, and only when the kernel's own loop found nothing to
kick.** DDR-1014's defect leaves `saw_idle=1` by construction — that loop *did*
see idle CPUs, it just spent its one attempt on the BSP — so it still FAILs.
That is M2 below, and it is the mutant the change stands or falls on.

---

## 4. A SECOND FINDING, FOUND BY NEEDING THE FIELDS TO BE TRUSTWORTHY: THE READ IS A USE-AFTER-FREE

Making the verdict depend on `g_rp_thread->dbg_ub_*` forced the question the
FAIL-branch print never had to answer: **is that TCB still there?** Measured in
the tree, not reasoned from the design:

* `sched_create` sets `t->parent_pid = 0` (`sched.c:1112`).
* `pid_alive(0)` returns **0** — its own comment: *"parent_pid 0 == kernel/none
  -> treat as orphan"* (`sched.c:1995`).
* the reaper reaps `THREAD_ZOMBIE && !t->waiter && !pid_alive(t->parent_pid)`
  (`sched.c:2017`) and calls `sched_free_tcb(victim)`.

So `resched_probe`'s TCB is **precisely** what the reaper frees, and the reaper
is already running: `sched_start_reaper()` is `main.c:3287`, `fs_test_thread` —
which contains this proof — is spawned eight lines later at `main.c:3295`.

The shipped window is **wide, not narrow**: the probe exits (setting `g_rp_ran`)
inside the `while (!g_rp_ran …) yield();` loop, and those `yield()`s are exactly
when the reaper gets to run; the FAIL branch then dereferences the pointer
afterwards.

**It did not fire in this capture, and that is checkable rather than assumed:**
`KHEAP_DEBUG` is unconditionally 1 and `cache_free` memsets the object to
`POISON_FREE` (`0xDD`, `kheap.c:22/174`), so a freed TCB would have printed
`kidle=221 kkick=221`. It printed `0`. The reading in §1 stands.

**Narrowed, not closed, and the residual is stated rather than papered over.**
The fields are copied into locals **immediately after `sched_unblock` returns**,
adjacent to the existing `idle_after` sample, with no `yield()` in between —
orders of magnitude tighter than a read taken ~50 ticks later, but not zero,
because another CPU can in principle run the probe to completion *and* schedule
the reaper *and* have it walk and free, inside those few instructions. Closing it
completely needs either a permanent zombie (one leaked TCB in the all-threads
ring for the life of the boot, visible to `ps` and walked by `pid_alive`) or
plumbing an out-parameter through a function `sched_unblock` is called from
MSI-X interrupt context — both worse than the residual. **Recorded, not built.**

### 4.1 THE VALIDITY GUARD IS NOT A FORMALITY — IT IS DDR-1077 §3.3 ONE LEVEL DOWN

DDR-1077 found that a bare conjunction reported success when the measurement
broke (zero `cr3` writers), and made zero an unconditional failure: *"fix the
pattern, do not adjust the pin."* The same trap is live here and is **worse**,
because the verdict would be trusting a byte read out of possibly-freed memory.

The guard is cheap and exact: both fields are `uint8_t` written **only** as 0 or
1 (`sched.c:1895-1907`), so **any value above 1 means the read is not
trustworthy** — a poisoned TCB reads 221, a recycled one reads whatever the new
owner put there (`kmalloc` does not zero, §NON-NEGOTIABLE 10). The SKIP path is
taken only when the pointer is non-NULL **and** the pair is in range **and**
`kidle == 0`. Everything else falls through to FAIL, which is the safe
direction, and `kvalid=` is printed so a reader is never left inferring from
`221` whether the verdict trusted its own input. That is DDR-883's rule —
*"both terms are printed on failure because [they] demand opposite actions"* —
applied to the verdict's input rather than its output.

---

## 5. A THIRD, SMALL CORRECTION: THE SKIP BRANCH'S STATED PROPERTY IS FALSE FOR ONE GATE

DDR-1004's comment claims:

> SKIP carries neither "OK" nor "FAIL", so it **trips no gate sentinel** and no
> `GLOBAL_FORBIDDEN` entry

The second half is true. The first half is false and one grep settles it:
`smoke-resched` (`Makefile:4106`) declares `EXTRA_SENTINEL="[smp] resched OK"`,
a **required** pattern, so a SKIP fails that gate.

**That is correct behaviour and is not changed.** `smoke-resched` exists to test
the kick; a boot that never exercised the kick must not pass it — which is
DDR-1004's own stated reason for inventing SKIP (*"OK would silently stop
testing the kick"*). Only the sentence describing it was wrong.

**So the trade this change makes, stated plainly rather than discovered later:**
a `kidle=0` boot stops reddening *whichever of the 179 gates happened to boot*
under a `GLOBAL_FORBIDDEN` entry that reads as a scheduler defect, and instead
reddens **`smoke-resched` alone**, as a missing required sentinel, with a line
that names itself as a coverage gap. The blast radius drops from any gate to the
one gate that owns the claim. It does **not** become invisible, and no gate is
made to pass on a boot that did not exercise rq-3.

---

## 6. NO NEW GATE, AND THE OBVIOUS ARM IS VACUOUS — MEASURED BEFORE IT WAS WRITTEN

Tenth time this is caught in design text. The natural arm is *"assert the gate
no longer prints `resched FAIL`"*, and it **passes on the unfixed tree in the
overwhelming majority of boots** — the artefact is a rare intermittent, three
occurrences across the project's whole CI history. An assertion that the absence
of a rare event is now guaranteed is unfalsifiable in any number of runs this
project can afford, and DDR-1082 already costed exactly that shape and refused
it.

Nor can the SKIP line be asserted conditionally-on-appearance: an arm that only
checks a line *when it is present* cannot fail, which is the dead-arm class this
project has now caught more than a dozen times.

So the proof is **forced mutants on recorded kernel hashes** — the DDR-1030
(`idle2=`) and DDR-1047 (lock dump) standard for an instrument whose triggering
condition cannot be manufactured in product. **179 gates unchanged.**

---

## 7. MUTANTS — MEASURED, AND THE STRUCTURE IS FORCED BY THE CODE

**A constraint discovered while building them, recorded because it shapes every
row: the branch that reads these fields is only reachable when NO KICK WAS
DELIVERED.** The `OK` arm tests `g_resched_ipis > before` and is evaluated before
anything consults `kidle`, so a mutant that only rewrites the recorded fields
still prints `OK` and proves nothing. Every mutant must therefore first suppress
the delivered kick. **M2 is that base — one change — and M1 and M3 are each
exactly one further change from M2**, so every comparison is pairwise
single-variable and DDR-1042's objection to attributing from a two-thing
mutation does not arise.

Baseline (unmutated, `6343bf987c60ee96`): `smoke-resched` **rc=0**,
`smoke-smplock` **rc=0**, `[smp] resched OK` — the change is a **no-op on a
healthy boot**, which is the first thing it had to be.

| | kernel | mutation | measured |
|---|---|---|---|
| **M0** | `4893d4ed2c6e664c` | **the PRE-FIX verdict** + M1's kernel state | `resched FAIL ipis=0 ran=1 idle=1 idle2=1 kidle=0 kkick=0`; `smoke-smplock` **RED** (`matched: resched FAIL`) |
| **M1** | `87948604d99dac1b` | base + `saw_idle` never recorded — *no kick owed* | `resched SKIP no-idle-ap ran=1 idle=1 kidle=0`; `smoke-smplock` **rc=0 PASS**; `smoke-resched` **rc=2**, `required pattern '[smp] resched OK' not found` |
| **M2** | `959d26a3508465be` | base alone — *a kick owed and missed* | `resched FAIL ipis=0 ran=1 idle=1 idle2=1 kidle=1 kkick=0 kvalid=1`; `smoke-smplock` **RED** |
| **M3** | `31835d3c08e1d9f4` | base + `saw_idle`=0, `kicked`=`0xDD` — *a recycled TCB* | `resched FAIL ipis=0 ran=1 idle=1 idle2=1 kidle=0 kkick=221 kvalid=0`; `smoke-smplock` **RED** |

**M0 IS THE WHOLE ARGUMENT AND IT IS NOT SYNTHETIC IN THE PART THAT MATTERS.**
Its line is byte-identical to the CI capture in §1 —
`ipis=0 ran=1 idle=1 idle2=1 kidle=0 kkick=0` — and it reddens the same
unrelated gate. M1 is the *same kernel state* under the fixed verdict and gives
SKIP with `smoke-smplock` green. Same state, two verdicts, both measured: that
is the claim, rather than a reading of the diff.

**M2 is what stops this being coverage deletion.** Without it, "the FAIL stopped
happening" and "the assertion was removed" are the same observation. It prints
`kidle=1 kkick=0 kvalid=1` and still FAILs — DDR-1014's defect class, intact.

**M3 is what stops §4.1's guard being a no-op, and it is load-bearing rather
than decorative: it prints `kidle=0`.** Without the range check that is exactly
the SKIP condition, so an unfixed guard would have *skipped on a byte read out
of freed memory*. The guard sees `kkick=221 > 1`, reports `kvalid=0`, and falls
through to FAIL. M1 and M3 differ by one line and land on **different verdicts**.

### 7.1 `-Werror` REJECTED MY FIRST M1, AND THE FIRST RUN OF IT WAS DISCARDED

The first M1 assigned the recorded field a literal instead of the variable, and
`-Werror,-Wunused-but-set-variable` refused to build it (`sched.c:1895`). The
gate invocations that followed therefore ran **the previous mutant's kernel**,
still sitting in `build/`. Caught only because the hash was printed and did not
appear; the results were discarded and M1 was rewritten to keep the variable
live. Recorded rather than quietly redone — it is DDR-1066's observation
(`-Wunused-function` catching a mutant that dropped an error path) arriving
again, and the reason a mutant run must print its kernel hash rather than assume
`make` succeeded.

### 7.2 A LIMITATION OF THE GUARD, STATED RATHER THAN LEFT TO BE FOUND

The range check catches the **poison** pattern and any garbage above 1. It
**cannot** catch a recycled TCB whose two bytes both happen to be 0 — that is
indistinguishable from a genuine "no kick was owed", and no check on these two
bytes could tell them apart. What bounds it is §4's narrowing: the read now
happens with no `yield()` between it and `sched_unblock`, so the reuse would
have to complete inside a few instructions. Not zero, and not claimed to be.

## 8. NOT CLAIMED

* **NO scheduler defect is named or fixed.** The kick path, the counter and
  DDR-1014's fix are all correct and untouched. What changes is which of three
  verdicts a *correct* kernel is reported under.
* **OPEN-2 IS NOT TOUCHED.** This is not an `[apfreeze]`, no CPU froze, no panic
  occurred, and no open issue moves (OPEN-1/2/12/13 unchanged).
* **The convicting reading is still ambiguous.** `kidle=1 kkick=0` is left
  exactly as DDR-1074 left it, and the third field it designed and refused is
  still not built.
* **The UAF is narrowed, not closed** (§4), and the two ways to close it are
  named and refused rather than left implicit.
* **`idle=`/`idle2=` are kept.** They are racy and now decide nothing, but they
  are what the two earlier captures were read with, so removing them would make
  this DDR's own §1 unreadable against the record.
* **NO rate is claimed** from three occurrences, and no campaign was run to
  manufacture one.
* **NO new gate** (179 unchanged); `GLOBAL_FORBIDDEN` **76 unchanged** — no
  pattern is added or removed, and `resched FAIL` still reddens every gate when
  it is printed. What changes is when it is printed.
