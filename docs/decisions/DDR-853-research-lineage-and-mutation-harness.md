# DDR-853 — hypothesis tree, genome lineage, dead-end registry, and a mutation harness that fails closed

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #60 (hypothesis tree), #61 (`genome.md`), #63 (dead-end
registry), plus `tools/mutation/mutate.py`. Host-side Python, no kernel surface.

## Part 1 — the three research structures

All three are **records of things that did not work**, which is the point. A
research agent's failures are its most valuable output and the easiest to lose:
a refuted hypothesis looks like clutter, a superseded genome looks obsolete, a
dead end looks like noise. Delete them and the agent re-derives the same wrong
answer indefinitely, at full cost each time, with nothing in the record
explaining why it keeps happening. So every structure is **append-only** and
every one retains what failed.

### #60 hypothesis tree

- **A hypothesis requires a prediction.** One that cannot be wrong cannot be
  right either — an experiment against it "confirms" it whatever happens.
- **Superseding creates a new version; the old one remains.** Editing in place
  erases the reasoning that led somewhere wrong, which is the only thing that
  stops the agent walking back down it.
- **Resolution requires evidence**, refutations included. An unevidenced verdict
  is an opinion recorded in the shape of a result.
- **A resolved hypothesis cannot be re-resolved** — that would overwrite the
  first verdict and the evidence behind it.
- **An unknown serialisation version is refused**, not best-effort parsed. The
  tree persists across boots; guessing at a format this build does not know is
  how a persisted structure silently loses fields.

**On the absent cycle check.** One was written and then removed. Two invariants
make it unreachable: a parent must exist when its child is added, and no node is
ever re-parented (`add` refuses a duplicate id; `supersede` creates a *new*
node). Every edge therefore points strictly backwards in insertion order.
Keeping the check would have been dead code that *looks like* a safety net —
worse than no net, because it invites the reader to trust a guard that has never
run and would not be noticed if it broke. The acyclicity argument is asserted by
a test instead.

### #61 `genome.md`

**A mutation requires a rationale.** A change with no stated reason records
*that* something changed and not *why* — and why is the part that tells you
whether to do it again. An empty generation (no trait change) is refused: it
makes the lineage longer without making it more informative.

### #63 dead-end registry

**A dead end requires a failure reason.** "It did not work" cannot be checked
against a new attempt, so the registry grows while remaining unable to prevent
anything.

`check()` returns **the entry**, not a boolean, so a refusal can show the
operator what it collided with and why that failed. A bare `False` makes the
refusal unarguable.

Divergence is `1 - Jaccard` over tokens — deliberately simple and deterministic.
An embedding score would judge meaning better and would make a refusal
impossible to explain; "the model thought it was similar" is not a reason an
operator can argue with, and this refusal must be arguable.

## Part 2 — the mutation harness, and two defects in my own verification

Every DDR from 847 onward leans on a mutation table: flip the guard off, and if
no test dies, the guard is not verified. The ad-hoc shell harness used for those
slices had **two defects that made it report success it had not earned**, both
found while running this slice.

**1. Stale bytecode misattributed kills.** `__pycache__` on `/mnt/c` has coarse
mtime granularity, so a mutate/restore cycle inside one second could leave
Python importing the *previous* module. Symptom: a mutation appeared to kill a
test that had nothing to do with it.

**2. A mutation whose target string was absent was SKIPPED with a warning, and
the run still printed kills.** DDR-850 reflowed a `validate_skill_update(...)`
call onto three lines, so one guards-matrix mutation silently stopped applying —
and the "kill" it reported was stale bytecode from the mutation before it.

That second one is **instance 16** of this project's recurring structural
defect: a check that absorbs an invalid input instead of rejecting it, so drift
is silent and looks like success — this time inside the tool built to detect
that very defect.

**Both were remediated before this commit, and every prior matrix was re-run**
with `__pycache__` cleared and `-B`:

| matrix | mutations | result on re-run |
|---|---|---|
| skillopt guards (DDR-848) | 5 | all killed — M4 re-run with the corrected target kills 2 |
| budget (DDR-849) | 10 | all killed, attributions unchanged |
| spec corrections (DDR-850) | 7 | all killed, attributions unchanged |
| goals/subconscious/MOSS (DDR-851) | 11 | all killed, attributions unchanged |
| routing/OCR (DDR-852) | 10 | all killed, attributions unchanged |
| research (this DDR) | 10 | all killed; one attribution corrected by the fix |

No guard was found unverified. The conclusions of DDR-848 through 852 stand —
but they stood on a harness that could have hidden a gap, and that is worth
recording rather than quietly re-running.

`tools/mutation/mutate.py` replaces the shell version and **fails closed**:

- a missing target **aborts** (exit 2) instead of warning
- an ambiguous target (>1 occurrence) aborts rather than guessing
- bytecode is cleared before every run and imports use `-B`
- **a mutation that kills nothing fails the run** (exit 1). A surviving mutation
  is the finding, not a footnote.

## Verification

`aether/tests/test_research.py` — 32 tests. Ten mutations, all killed.
Section 3D is now **18 of 21** (remaining: #62 vector knowledge graph,
#64 population tournament, #65 run visualiser).

(Locally verified, not CI-confirmed — outage `qcvjkzcs7j74` open. Committed
without pushing per the operator's instruction.)
