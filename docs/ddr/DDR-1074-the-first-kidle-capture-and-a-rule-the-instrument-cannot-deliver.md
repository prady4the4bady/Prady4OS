# DDR-1074 — DDR-1064's fields printed on a real failure for the first time, and
# they EXONERATE; but the rule beside them convicts on a reading a CORRECT
# kernel produces, and the convicting reading is NOT deliverable at all

**Date:** 2026-09-06
**Status:** measured + comment corrected. **No behaviour change; `kernel.bin`
bit-identical (comments only).**
**Class:** DDR-1046 / DDR-1070 shape — a control that cannot see the case it
claims to. Third instance in this one lineage (DDR-1030's race named by
DDR-1064; DDR-1064's own rule corrected here).

---

## 1. The artefact — the capture DDR-1064 said did not exist yet

CI **34023281940**, `build-and-boot (shard 7)`, tip **`0da019f`**, gate
**`smoke-smp`** (5th of 17 on that shard):

```
[smp] resched FAIL ipis=0 ran=1 idle=1 idle2=1 kidle=0 kkick=0
```

- **`0da019f` is DOCS-ONLY** — `git show --stat` is one file, `SESSION_HANDOFF.md`,
  39 insertions. `kernel.bin` cannot have changed, and the shard's own post-gate
  assertion printed **`kernel.bin: OK`** (DDR-1035). DDR-1009 class.
- **The gate is incidental**, exactly as DDR-1064 recorded: the rq-3 proof runs on
  every boot and `resched FAIL` is in `GLOBAL_FORBIDDEN`, so it reddens whichever
  SMP gate boots first on a shard. DDR-1064 saw `smoke-smppreempt` (shard 4) and
  `smoke-smpsched` (shard 7); this is `smoke-smp` (shard 7). Do not read the gate
  name as locating anything.
- The 40 lines of context are a **healthy boot**: FAT + SFS phases complete,
  `[smp] cross-wake OK`, `[smp] sched cross-CPU OK`, `[smp] ap preempt OK`
  immediately before the FAIL.

**This is the first `resched FAIL` ever to carry `kidle=`/`kkick=`.**
`PRE_LAUNCH_CHECKLIST.md` recorded the absence in as many words: *"FIRST RESULT
ON THE INSTRUMENTED KERNEL IS A NEGATIVE … no `kidle=`/`kkick=` capture exists
yet."* It exists now.

### 1.1 What it says

**`kidle=0` — and that reading is sound.** `sched_unblock`'s own loop, at the
instant it ran, saw **no idle non-self CPU at all**. No kick was owed, so
`ipis=0` is correct, and `ran=1` says the property under test held. `idle=1
idle2=1` are the proof's samples taken at different instants and are the racy
terms DDR-1064 identified.

**Consistent with a correct kernel.** No fix, and §NON-NEGOTIABLE 3 forbids one:
there is no defect named here.

---

## 2. THE FINDING: the rule printed beside the line is wrong, and its own commit says so

`main.c` documents the discrimination immediately above the print:

> `kidle=1 kkick=0` -> the kernel saw an idle CPU and delivered nothing:
> **the only reading that convicts the scheduler.**

`sched.c:1848-1854`, in the function that **records the field**, states the
opposite, in the same commit:

> `saw_idle` deliberately mirrors THIS loop's predicate exactly, not the
> proof's: the proof carries `!o->is_bsp` because `smp_resched_one` declines the
> BSP … so **a BSP-only-idle boot reads `saw_idle=1 kicked=0` and is CORRECT
> rather than a defect**.

**The tree settles it.** `smp.c:310`:

```c
int smp_resched_one(uint32_t cpu_idx) {
    struct percpu *pc = percpu_get(cpu_idx);
    if (pc && pc->present && !pc->is_bsp) { ...; return 1; }
    return 0;                       /* the BSP is deliberately never kicked */
}
```

and the recording loop carries **no `!is_bsp` filter**:

```c
if (c != self && o && o->present && o->idle) {
    saw_idle = 1;
    if (smp_resched_one((uint32_t)c)) { kicked = 1; break; }
}
```

So when the only idle non-self CPU is the BSP: `saw_idle = 1`,
`smp_resched_one` returns 0, nothing is delivered, `kicked = 0`. **A completely
correct kernel prints `kidle=1 kkick=0`.**

**`sched.c` is right and `main.c` is wrong**, and it is wrong in the dangerous
direction: it instructs a future session to convict the scheduler on the reading
that BSP-only-idle produces — which is the *exact predicate mismatch DDR-1014
spent a whole DDR removing*, re-armed one level up as a diagnostic instruction
rather than as a verdict.

---

## 3. AND THE CONVICTING READING IS NOT DELIVERABLE — a third field was designed and REFUSED

The obvious repair is a third field: record whether an idle non-self **non-BSP**
(i.e. kickable) CPU was visible, so `kidleap=1 kkick=0` would be unambiguous.
Recorded here because it looks right and is not, and the reason is only visible
by reasoning the mutation through **before** writing it:

**The mutation that would have to prove the arm is DDR-1014's own defect** —
restore `break` on the *call* rather than on a *delivered* kick. Walk it:

1. The loop reaches the idle BSP first, sets `saw_idle_ap`… **no** — the BSP is
   not an AP, so `saw_idle_ap` stays 0.
2. It calls `smp_resched_one`, which returns 0, and the mutant **breaks anyway**.
3. The idle AP later in the list is **never reached**, so `saw_idle_ap` is never
   set.

The mutant prints **`kidleap=0 kkick=0`** — which under the new rule reads
*"no kick was owed, artefact"*. **A false exoneration of precisely the defect the
field was added to convict.**

The field records what the loop **saw**, and a loop that stops early does not see
what it skipped. Making it sound needs a **post-loop** scan for a still-idle
kickable CPU — which is DDR-1030's race reintroduced verbatim (a CPU can enter
idle after the loop returns), the very thing DDR-1064 was written to remove.

**So the instrument cannot deliver a convicting reading from inside
`sched_unblock` without reintroducing one of the two races it exists to close.**
The third field is **not built**. A `smp_resched_eligible()` refactor (one
predicate shared by `smp_resched_one` and the recorder, removing the `!is_bsp`
duplication) was also designed and is **not built**, because it would only make a
field that still cannot discriminate more tidily wrong.

---

## 4. What the fields CAN say, which is what the comment now says

| reading | verdict | sound? |
|---|---|---|
| `kidle=0` | no idle non-self CPU was visible; **no kick was owed; the FAIL is a sampling artefact**, whatever `idle=`/`idle2=` say | **yes** — this capture |
| `kidle=1 kkick=1` | a kick WAS delivered; `ipis=` disagreeing means the **counter**, not the kick, is the defect | **yes** |
| `kidle=1 kkick=0` | **AMBIGUOUS** — either a BSP-only-idle boot (**correct**, DDR-1014's own case) or a genuinely missed kick. **These fields cannot tell them apart, and §3 shows no third field recorded in that loop can.** Resolve it from the capture's `[hb]` heartbeats and `-smp` width, not from this line. | **no — corrected here** |

Two of the three documented readings were sound. The third was not, and it was
the one the comment called decisive.

---

## 5. The verdict is DELIBERATELY still unchanged

DDR-1064 §6 refused to change it *"on one capture"*, and this is one capture.
That refusal stands unaltered — with one distinction worth recording so the next
session does not re-derive it:

DDR-1064's stated objection was specifically to **collapsing this case to SKIP**,
because *"a genuinely broken kick also prints `ran=1`"*, so SKIP would delete
DDR-1014's coverage. A narrower change — **FAIL only on the convicting reading**
— would not have had that problem and was seriously considered here. **§3 is why
it was not built:** there is no sound convicting reading to gate on. The refusal
now rests on a measured limitation rather than only on sample size.

**Consequence, stated because it bears on the release:** `resched FAIL` is in
`GLOBAL_FORBIDDEN`, so a **correct** kernel can redden any gate on any shard at
some unmeasured rate, and that directly degrades the 3-green criterion
§NON-NEGOTIABLE 1 and the promotion depend on. Two occurrences are now on record
(DDR-1064's `e9ed2c9`, this one) and **no rate has been measured** — DDR-1062's
42-suite window recorded zero `resched FAIL` among its four attributed reds, so
these are the first two. **Not a rate; two occurrences.**

---

## 6. NOT CLAIMED

- **No kernel defect is named or fixed**, and no scheduler change is made. The
  capture is consistent with a correct kernel.
- **No behaviour change at all**: the correction is to a comment, so `kernel.bin`
  is **bit-identical** (`2c4868b2f5f0d00a` before and after) — verified, not
  assumed, and the appropriate proof for a comment-only change.
- **The verdict is unchanged**, so this gate can still redden on a correct
  kernel; §5 says why that was not fixed rather than leaving it implied.
- **No rate is measured** and none is inferred from two occurrences.
- **The third field and the `smp_resched_eligible()` refactor are not built**,
  and §3 records the mutation walk that refused them so the next session does not
  spend the change discovering the same thing.
- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched;
  this is not an `[apfreeze]` and is not OPEN-2.
- **No gate was re-run**, locally or in CI, for this DDR.
