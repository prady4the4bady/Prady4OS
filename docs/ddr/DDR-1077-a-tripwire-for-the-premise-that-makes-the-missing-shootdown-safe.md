# DDR-1077 — a tripwire for the premise that makes the missing TLB shootdown safe

**Status:** ACCEPTED
**Date:** 2026-09-06
**Closes:** the gap DDR-1075 §3.2 named and did not close.

---

## 1. What DDR-1075 §3.2 left open

DDR-1075 §3 measured that this kernel has **no cross-CPU TLB invalidation at
all**, and that the absence is **correct today** on four facts. The third is
the fragile one:

> **(c) No two threads share an address space.** The only `->cr3` writers are
> `elf.c` (a fresh AS), `sys_exec.c` (a fresh AS), `sched.c:1105` (`0` = the
> kernel master) and `sched.c:1239` (fork's fresh child). There is no
> `CLONE_VM` — DDR-1038 established that.

And it recorded the consequence:

> **Group D's `pthread`/`CLONE_VM` row deletes that premise**, after which every
> `vmm_unmap` (`sys_mmap.c:67`, `sys_surface.c:388`/`:448`) and every
> `vmm_protect_range` leaves a stale writable translation on another CPU —
> silent, timing-dependent, on the same SMP paths OPEN-2 lives in — **and
> nothing in the tree would notice**: no assertion, no counter, no gate.

That last clause is the defect this DDR closes. **The warning lived only in a
document**, and DDR-1071 §4 measured what that produces: *"correction is a side
effect of adjacent work rather than a process, and a row whose work finished
cleanly and drew no follow-up is exactly the row that stays stale."* A
prerequisite recorded only in prose is a prerequisite that will be missed.

**This does NOT build a shootdown.** It builds the noticing.

---

## 2. The invariant, and why it is pinned on `->cr3` rather than on `CLONE_VM`

The obvious check is `grep CLONE_VM`. It is the wrong instrument, for the
reason DDR-1073 §5 recorded about line numbers: it pins a **spelling** rather
than the property. A future session could share an address space through
`sched_create_thread(parent_as)` or any other name, and a `CLONE_VM` grep would
sit there green.

**Every way of sharing an address space must assign a `cr3` somewhere.** So the
tripwire pins the *set of `cr3` assignment sites*, which is the actual carrier
of fact (c). Measured, not carried:

```
$ grep -rnE '(->|\.)cr3[[:space:]]*=[^=]' kernel user --include='*.c' --include='*.h'
kernel/exec/elf.c:315          t->cr3 = as;
kernel/syscall/sys_exec.c:137  t->cr3        = new_as;
kernel/proc/sched.c:1105       t->cr3 = 0;
kernel/proc/sched.c:1239       t->cr3         = child_cr3;
```

**Pinned per FILE and COUNT, never by line number** — `kernel/exec/elf.c 1`,
`kernel/proc/sched.c 2`, `kernel/syscall/sys_exec.c 1`. DDR-1073 §5 is explicit
that *"a row citing line numbers has an expiry date and nothing in the tree can
check one"*, and this check must not acquire one.

---

## 3. The condition is a CONJUNCTION, and that is the whole design

```
FAIL  iff  (the cr3-writer set differs from the pin)
      AND  (no cross-CPU TLB invalidation exists in the tree)
```

Single-term versions are both wrong, and each is wrong in a way this project
has already been bitten by:

| variant | why it is wrong |
|---|---|
| "a new `cr3` writer is banned" | It reddens on **correct** work — a session that builds the shootdown *first* and then `CLONE_VM` would be blocked by the very check meant to protect that ordering. This is DDR-1071 §5's refused shape: a rule that fires on correct in-progress state gets removed. |
| "a shootdown must exist" | It fails **today**, on a tree where the absence is correct, so it would be silenced on day one. |

The conjunction permits every correct state and forbids exactly the dangerous
one. Truth table, all four cases stated rather than implied:

| cr3 writers | shootdown | verdict | why |
|---|---|---|---|
| pinned (4) | absent | **PASS** | today's tree; the absence is safe on DDR-1075 §3.1 |
| changed | absent | **FAIL** | the premise may be gone and nothing else would notice |
| pinned (4) | present | **PASS** | someone built the prerequisite; nothing to warn about |
| changed | present | **PASS** | the correct order — prerequisite first, then the sharing |

### 3.1 It fires in BOTH directions, deliberately

A *removed* writer is harmless, and the check fails on it anyway. That is a
choice, not an oversight: **a tripwire that only fires one way is half a
tripwire**, and the cost of the other half is one line of pin maintenance
while the benefit is that any change to the carrier of fact (c) gets looked at.

### 3.2 The shootdown term is weak, and its weakness is in the SAFE direction

"A shootdown exists" is `grep -i shootdown` over the tree — the same measurement
DDR-1075 §3 used to establish the absence. If someone builds one under another
name, the check keeps firing on new `cr3` writers: **annoying, not dangerous**.
Stated rather than discovered later.

### 3.3 A third clause, found while building §6's fixture 5 — the bare conjunction was WRONG

The truth table above is complete for a tree the grep can still read. It is not
complete for a tree the grep can no longer read, and fixture 5 is what exposed
that: under the bare conjunction, **ZERO `cr3` writers plus any file containing
the word "shootdown" reports SUCCESS**. Zero does not mean the tree changed —
it means **the measurement broke**, which is precisely the `GLOBAL_FORBIDDEN`
catastrophe at `89f71cc` (§NON-NEGOTIABLE 6), where an empty list failed
nothing and simply stopped catching, unnoticed for four commits.

So zero writers is an **unconditional** failure, checked before the
conjunction, with a message that says *fix the pattern, do not adjust the pin*.
Fixture 5 plants a shootdown deliberately, so it proves the clause is
unconditional rather than riding on the conjunction it precedes.

This is recorded rather than quietly fixed because the design was written down
first and the design was incomplete: the check that guards a premise had a
state in which it could not see anything at all.

---

## 4. What this check is NOT

* **It is not a verdict.** It cannot tell whether a new `cr3` writer actually
  shares an address space — that is semantic, and DDR-1071 §5 and DDR-1072 §2
  both established that nothing in the tree can read a semantic claim. It says
  *"the thing DDR-1075 §3.1(c) rests on has moved; go and look"*, and its
  failure message says exactly that.
* **It is not the dead-arm class**, though it passes on today's tree forever if
  nothing changes. That is the same shape as `ci-probe-rodata-check` and
  `ci-start-align-check`: a **guard**, whose liveness is proved by fixtures
  rather than by firing in production. §6's fixtures are what make that claim
  checkable rather than asserted.
* **It does not touch the kernel.** `kernel.bin` is unchanged.

---

## 5. Implementation

`tools/ci/cr3_writers_check.sh` plus `tools/ci/cr3_writers_selftest.sh`, behind
one `make ci-cr3-writers-check` that runs both — the check on the real tree,
then the fixtures. Wired into `tools/ci/hygiene_check.sh` (**ALL SEVEN → ALL
EIGHT**) per that script's own rule that a list of names drifts and the script
cannot, and into `.github/workflows/ci.yml`'s `shard-check` job, which is where
the other three toolchain-free static checks already run.

`TREE_ROOT` is overridable so the fixtures point it at a synthetic tree rather
than mutating the real one. Fixture trees are built under `build/`
(§NON-NEGOTIABLE 7 — never `/tmp`).

**The selftest is not optional decoration.** This guard passes on today's tree
and will keep passing forever if nothing changes, so "the check is quiet" and
"the check is dead" are the same observation from outside — the dead-arm class.
The fixtures are the only thing that separates them.

---

## 6. Fixtures — the three that must FAIL are the load-bearing half

| # | tree | expect | what it proves |
|---|---|---|---|
| 1 | the REAL tree | PASS | the pin matches reality (and so the check is not reporting on a tree it cannot see) |
| 2 | a 5th `cr3` writer, no shootdown | **FAIL** | the dangerous case — this is the whole point |
| 3 | a 5th `cr3` writer **and** a shootdown | PASS | the correct ordering is permitted; without this the check would block the work it guards (§3) |
| 4 | pinned set, no shootdown | PASS | vacuity: it does not simply always fail |
| 5 | **ZERO** `cr3` writers, **with** a shootdown planted | **FAIL** | the pattern-broke case. A check whose grep stops matching must not report success — that is exactly the `GLOBAL_FORBIDDEN` catastrophe at `89f71cc` (§NON-NEGOTIABLE 6), where an empty list failed nothing and just stopped catching. The planted shootdown is what makes this fixture prove §3.3's clause is **unconditional** rather than riding on the conjunction: under the bare conjunction this tree PASSES. |
| 6 | a writer MOVED between files (total unchanged) | **FAIL** | per-file counts, not a total — see M2 |

## 7. Mutants

| | mutation | must fail |
|---|---|---|
| **M1** | drop the shootdown term (any change to the set fails) | fixture **3** — a correct combination reddened |
| **M2** | compare the TOTAL count instead of per-file counts | fixture **6** — a writer moved between files slips through |

They must fail **different** fixtures, or the fixtures are not independent —
the DDR-1044 M2/M3 check.

**MEASURED, both directions.** Baseline: all six fixtures land as specified
(`3 must-pass + 3 must-fail`). **M1** (`shootdown -eq 1` → `-eq 99`, i.e. the
term can never be satisfied) fails **fixture 3 alone** — the correct ordering
reddened, and fixtures 1/2/4/5/6 are untouched. **M2** (`actual` becomes
`wc -l` of the hits and the pin becomes the literal `4`) fails **fixture 6
alone** — `sched.c 2 → 1` with `elf.c 1 → 2` keeps the total at four and slips
straight through, and the check prints `set unchanged` about a tree whose set
plainly changed. Reverting each returns the baseline six-for-six.

---

## 8. Not claimed

* **No TLB shootdown is built**, and none is designed here. DDR-1075 §3.1's
  four facts still make the absence correct, and this changes nothing about
  that.
* **No defect is fixed.** There is no present bug — fact (c) holds today, and
  this guards a *future* change.
* **It cannot detect address-space sharing semantically** (§4); it detects that
  the carrier of the premise moved.
* `kernel.bin` untouched; 177 gates unchanged; no open issue moves.
