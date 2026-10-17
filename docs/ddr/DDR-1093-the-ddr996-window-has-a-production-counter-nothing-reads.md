# DDR-1093 — THE DDR-996 WINDOW HAS A PRODUCTION COUNTER AND NOTHING READS IT

Status: IMPLEMENTED (one heartbeat field), no new gate, no sentinel
Date: 2026-09-07
Scope: the ONE remaining genuinely-open Group A row.

---

## 1. THE ROW, AND WHY ITS GATE COLUMN IS THE SHAPE THAT KEEPS BEING WRONG

Group A: **"Per-CPU `sched_exit` / zombie reap under full SMP"**, gate column
**"existing SMP gates"**. DDR-1073 established this is one of only two Group A
rows still genuinely open, and DDR-1082 then costed and refused the other
(`smoke-rqstress` 20×), so this is the last one.

"Existing SMP gates" is not a gate name; it is a claim that coverage falls out
of gates written for other things. That is the DDR-1072 §2 / DDR-1073 §2 shape,
and both times it was measured it did not hold.

---

## 2. WHAT IS ACTUALLY THERE, MEASURED

DDR-996 fixed the defect this row is about: `sched_exit` marks a thread ZOMBIE
and leaves its per-CPU FIFO link intact, both reap paths unlinked only the
all-threads ring, so a thread reaped before some `rq_take()` popped it was freed
while a queue still pointed at it (the `fair_candidate` `#GP` on `PMM_POISON`,
CI 32702096039). The fix unlinks in `sched_free_tcb`.

Three facts, each read in the tree:

1. **The fix's counter runs on the real path, everywhere.**
   `sched_free_tcb` (`sched.c:1324`) guards the unlink on
   `__atomic_load_n(&t->rq_on)` and increments `g_rqfree_caught` (`:1336`)
   inside the match. So **`caught` is incremented exactly when the DDR-996
   window occurred** — on every boot, on every CPU, on both reap paths.

2. **`rq_references()` — the invariant check — is called from EXACTLY ONE
   PLACE**, `sched.c:2079`, inside `sched_rqfree_probe`. `g_rqfree_leaked` has
   one writer (`:2080`, in that probe) and one reader (`main.c:1875`, that
   probe's own report).

3. **That probe is driven by one gate, and it is SINGLE-CPU.**
   `grep -n "QEMU_PROBES=rqfree" Makefile` returns exactly one line
   (`smoke-rqfree`, `Makefile:3244`, shard 9 strict), and its recipe sets **no
   `QEMU_SMP`** — measured, `grep -c QEMU_SMP` over the recipe returns 0.

**So: the number that says whether this race arises in production is computed
correctly on every SMP boot and read by nothing.** `main.c:1866` reads it only
inside `if (probe_enabled("rqfree"))` — i.e. only on the one gate where the
window is *manufactured deterministically* and the machine has one CPU.

### 2.1 THE PROBE'S OWN RATIONALE NAMES A CONDITION ITS GATE CANNOT PRODUCE

`main.c:1861` explains why arm A asserts `caught > 0` and not `caught == N`:

> IRQs off across the unblock/destroy pair so this CPU cannot reschedule into
> the victim. **Another CPU may still steal and run it**, which is why the gate
> asserts `caught > 0` … an exact count would be asserting the absence of work
> stealing.

That reasoning is correct and it is about **work stealing**, which needs a
second CPU. On the one-CPU boot the gate actually runs, no other CPU can steal,
so the tolerance is real and the condition it tolerates never occurs. Not a
defect in the gate — it is right to be written that way — but it does mean the
gate's stated model of itself is an SMP model and its execution is not.

---

## 3. WHAT IS BUILT HERE, AND THE PRECEDENT IS EXACT

One field in the `[hb]` heartbeat: `rqfree=<g_rqfree_caught>`.

**`ymask=` in the same block is the same shape, added by DDR-981 for the same
stated reason**, and its comment is the argument for this one verbatim:

> yields that arrived with IF already masked — i.e. how often the interrupt
> window in `yield()` was **actually needed**. This is **the denominator for
> "the fix is exercised"** (R17): a gate asserting no `[apfreeze]` proves
> nothing if `ymask` stayed 0. One fixed-width numeric read of one global.

`rqfree=` is the denominator for DDR-996 in precisely that sense. Today, "no
`fair_candidate` `#GP` since DDR-996" proves nothing about whether the window
still arises, because nothing counts it where it arises.

The heartbeat is the right place for a second reason: it is where the OPEN-2
investigation already reads (DDR-1019/1049/1064/1074/1079/1088 all read `[hb]`
fields), so this is not a sidecar nobody opens — the failure mode DDR-1043
measured on the QMP dump.

Cost: two `kputdec`-class reads of one global, once per 500 ticks, inside a
block that already prints ~15 fields under a lock it already holds.

---

## 4. THREE THINGS DELIBERATELY NOT DONE

**4.1 `leaked=` is NOT added.** `g_rqfree_leaked` has exactly one writer and it
is inside the probe, so outside `smoke-rqfree` the field **cannot be anything
but 0**. Printing it would be a field that reads like a live invariant check and
is a constant — the DDR-1059 shape, a control that reads stronger than it is.

**4.2 `rq_references()` is NOT called from `sched_free_tcb`.** That is the
change that would make the invariant checked in production, and it is refused on
measured cost: it walks up to 4096 entries **per runqueue, for all `PERCPU_MAX`
runqueues, each under that queue's lock**, on every TCB free. That is DDR-1047's
refused shape exactly — real work added to a scheduler path in a kernel whose
open defect (OPEN-2) is a timing-sensitive AP freeze, where an instrument can
*move* the bug rather than measure it. `caught` costs nothing because
`sched_free_tcb` already does that scan for the fix itself; the invariant check
would be a *second* scan bought purely for observation.

**4.3 `smoke-rqfree` is NOT switched to `QEMU_SMP=4`.** Tempting, and it would
make §2.1's rationale true — but it makes a **strict-tier** gate's `caught > 0`
arm depend on losing a work-stealing race, which the probe's own comment says it
cannot control. Turning a deterministic arm into a timing-dependent one on the
gate that guards a fixed defect, days from a release that needs three greens, is
the wrong trade. **Recorded as the buildable variant if the number in §3 ever
shows the natural window is common enough to assert on** — which is exactly the
measurement that does not exist today and that this change creates.

---

## 5. IT IS AN INSTRUMENT, NOT A SENTINEL — AND THE DIRECTION MATTERS

`rqfree=` must **not** go in `GLOBAL_FORBIDDEN`, and the reason is not caution:
**`caught > 0` means the fix WORKED.** It counts TCBs that reached
`sched_free_tcb` still queued *and were correctly unlinked there*. A non-zero
value is DDR-996 doing its job; the defect it replaced was silent.

That is the opposite polarity from `[apfreeze]` or `panic_stage=`, and reading
it the wrong way round would redden every SMP gate on a correct kernel — which
is the consequence DDR-1074 had to record about `resched FAIL` and DDR-1092 then
narrowed. Stated here so the next session does not "complete" this row by adding
a sentinel.

---

## 6. NO NEW GATE, AND THE OBVIOUS ARM IS VACUOUS — MEASURED BEFORE WRITING

Eleventh time this is caught in design text. The natural arm is *"assert
`rqfree > 0` on an SMP gate"*, and it is the DDR-1073 §2 / DDR-1068 `reaped=`
shape: **a correct kernel legitimately reports 0** whenever no reap happened to
land inside the window on that boot. Asserting it makes a gate fail on timing;
asserting `rqfree >= 0` asserts nothing.

And the inverse arm is worse: `rqfree == 0` cannot be required either, because a
boot that *does* hit the window is correct too.

So there is nothing here to gate, and **179 gates are unchanged**. What the field
buys is that the next SMP capture — every one, on every shard — carries the
number, so the question "does this window arise in production at all?" becomes
answerable by reading a log instead of by writing another probe.

---

## 7. PROOF, AND THE FIRST DATUM THE FIELD PRODUCED

The claim is narrow (a global that was already maintained is now printed), so the
proof is narrow and is stated rather than inflated.

**Read back from an SMP capture, not inferred from `rc=0` (DDR-1041).** A
4-CPU boot (`QEMU_SMP=4`, full window) prints the field at every heartbeat:

```
[hb] t=500   … ymask=356937   … rqfree=0
[hb] t=1000  … ymask=921014   … rqfree=0
[hb] t=1500  … ymask=1474234  … rqfree=0
[hb] t=2000  … ymask=2028948  … rqfree=0
```

**A method note worth carrying:** the first attempt at this read-back produced
an EMPTY result and it was the measurement that was wrong, not the field. With
no sentinel declared, `boot_test.sh` takes DDR-785's early exit the moment
`NEXUS KERNEL OK` appears — line ~30 of the boot — and the first heartbeat is at
`t=500`, so the capture was 1,713 bytes with zero `[hb]` lines. Declaring a
never-appearing `FORBIDDEN_SENTINEL` disables the early exit (DDR-1043's rule)
and the window runs. Recorded because "the field did not print" and "the boot
ended before the field could print" are the same observation from outside.

### 7.1 `rqfree=0` IS THE INTERESTING ANSWER, AND IT SETTLES §4.3 BY MEASUREMENT

On this boot the DDR-996 window **did not arise naturally at all**, while
`ymask` climbed past two million in the same interval — so the boot was busy and
the counter was simply not reached.

**ONE BOOT IS ONE OBSERVATION AND NO RATE IS CLAIMED.** What it does establish
is the thing §4.3 and §6 previously argued: an arm asserting `rqfree > 0` on an
SMP gate would fail on a correct kernel, and switching `smoke-rqfree` to
`QEMU_SMP=4` would therefore trade a deterministic strict-tier arm for a
timing-dependent one. That refusal now rests on a number rather than on
reasoning, which is the whole point of adding the field.

Regression: the SMP and scheduler gates stay green (§ below), hygiene ALL EIGHT,
`GLOBAL_FORBIDDEN` 76, and `kernel.bin` is **1,311,114 B — SIZE UNCHANGED**, so
the size/headroom pair and `ci-docstate-check` are unaffected.

**There is no mutant, and that is a stated limitation rather than an oversight**
— the same reasoning DDR-1080 recorded. A mutant that stops incrementing
`g_rqfree_caught` would prove only that the field prints what the global holds;
what the field is *worth* is decided by whether a future SMP capture ever shows
it non-zero, and no mutation can establish that. The counter's own correctness
is already covered two-sidedly by `smoke-rqfree`'s arm A (`caught > 0` required,
and `leaked == 0`).

## 8. NOT CLAIMED

* **NO defect is found and none is fixed.** DDR-996's fix is correct and
  untouched; `sched_exit`, both reap paths and the `on_cpu` handshake are
  unchanged. What changes is that one existing number becomes visible.
* **The Group A row is NOT closed.** It asked for coverage under full SMP and
  this is not coverage — it is the measurement that has to come first, because
  §6 shows there is nothing sound to assert until the natural rate is known.
* **NO rate is claimed.** Whether the window arises on an SMP boot at all is
  exactly the open question; this change is what makes it askable.
* **The invariant is still checked only in the probe** (§4.2), and the
  single-CPU limitation of `smoke-rqfree` is recorded, not removed (§4.3).
* **OPEN-1/2/12/13 are untouched**, no open issue moves, `GLOBAL_FORBIDDEN`
  stays 76 (§5 is why nothing is added), and no new gate (179 unchanged).

---

## 9. FOUND WHILE MEASURING §2 — A GATE NAME I GOT WRONG ONE COMMIT AGO

§2 needed `grep -n "QEMU_PROBES=rqfree" Makefile` to establish that
`smoke-rqfree` is the only driver of that probe. Having the two lists in hand
(every `smoke-*` named in `CLAUDE.md`, every `^smoke-*:` target in the
`Makefile`) made a wider comparison free, so it was run: **160 names claimed,
184 targets real, 63 claimed names with no target.**

Almost all 63 are correct. Most are the DDR-1063 §7c shape — a planning table
naming a gate for work not yet done (`smoke-prad`, `smoke-tap`,
`smoke-iso-aarch64`, the eleven Group F agent gates) — and several are recorded
non-existence this file already states in as many words (`smoke-wx`,
`smoke-mc`, `smoke-maximize`, `smoke-lazystack`, `smoke-vdso-read`,
`smoke-readline`, `smoke-jobctl`, `smoke-pipes`, `smoke-lockbox-e2e`,
`smoke-lockstat`, `smoke-capagent`). **One is not.**

**The Group F agent-respawn row said init's refusal is "gated by
`smoke-svc`". `smoke-svc` has never existed.** The real target is
**`smoke-init`** (`Makefile:1549`, `gate_shards.txt:73`, **shard 1, 27 s,
strict**), and the *claim* was correct — its `EXTRA_SENTINEL` list contains
`[svc] refuse agentsvc` as a **required** pattern, so the refusal really is
gated, at required-pattern strength.

**This is the DDR-1040 `smoke-wx` shape — a wrong name for shipped, gated work
— and I introduced it in DDR-1085, one commit earlier.** Carry two things from
it rather than only the fix:

1. **It is DDR-1083 §2 arriving in my own writing.** That finding was a pin in
   `aether.h` whose stated justification was false, *in the file whose entire
   job is to be the thing other files are checked against*. `CLAUDE.md` is that
   file for this project, and a gate name in it is exactly the sort of claim a
   future session reads without re-deriving (§INV.14's own correction describes
   the position: check the stated reason, find it does not resolve, and now
   decide which half of the document to believe).
2. **The mechanical check that would catch it cannot be built, for the reason
   DDR-1081 §3 already established** — and this sweep is a third demonstration.
   The signal is *"a named gate has no target"*, and it is a **defect** on this
   row while being **correct** on ~62 others. Nothing mechanical separates them;
   only reading the row does. So no checker is added, the comparison above is
   recorded as a thing worth re-running by hand when a table is edited, and the
   one wrong name is corrected in the same commit as the code.

**Not claimed:** the audit was `CLAUDE.md` only — `docs/BUILD_TRACKER.md`,
`docs/AETHER_MASTER_FEATURES.md` and `docs/PRE_LAUNCH_CHECKLIST.md` were not
swept, and the 63-name list was **not** individually adjudicated beyond
identifying the one defect; the other 62 are grouped by inspection, not
certified one by one.
