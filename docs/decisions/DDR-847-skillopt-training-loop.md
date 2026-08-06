# DDR-847 — SkillOpt: the agent skill training loop

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #46 (Group 2, item 9). Host-side Python (`aether/`), no
kernel surface, no new syscall.

## Process note (recorded, not back-dated)

Project rule is *ADR/DDR before the code it governs*. That order was **not**
followed here: `aether/agents/skillopt/__init__.py` was written first, and this
DDR was written after. Stating it plainly rather than dating this document
earlier, as was done for DDR-833. Nothing below was reverse-justified from the
code — the acceptance rule is the reason the module exists — but the sequence
was wrong and is on the record.

## Context

Section 3D #46 calls for a skill training loop:

    rollout -> reflect -> aggregate -> select -> update -> evaluate

An agent runs tasks, reflects on what happened, and its skill prompt is revised
from those reflections. The loop is only worth having if the revision is an
improvement. Everything hard about this is in the last stage.

## Decision

### 1. A candidate replaces the incumbent ONLY on a strictly positive held-out improvement

`>`, not `>=`. Enforced as an explicit branch with its own rejection message,
not as an implicit comparison.

**Why this is not pedantry.** An optimiser that accepts ties drifts. Every
accepted tie changes the skill without any evidence the change helped — it is a
coin flip dressed as an update. After N ties the skill has wandered somewhere
nobody chose and no measurement supports, and because each individual step
"passed", nothing in the history says so. Requiring `>` makes that drift
*impossible* rather than merely unlikely.

This is the same structural defect this project has now hit eleven times
(DDR-817, -822, -823, -824, -825, -826, -830, -832, -833, -835, -845): a check
that absorbs an invalid case instead of rejecting it, so drift is silent and
looks like success. A tie is the invalid case here.

### 2. Scoring is HELD-OUT, and disjointness is enforced rather than assumed

`run()` raises `ValueError` if the held-out set shares any `task_id` with the
training rollouts. A skill scored on the rollouts that produced it is scored on
its own homework — the metric-gaming failure S7 exists to prevent, and the
easiest way for this loop to look like it is working while getting worse.

Enforced, not documented: a convention that only lives in a docstring is one
careless caller away from being untrue, and the failure is invisible from the
outside because the numbers still go up.

### 3. Below `MIN_HELDOUT_ROLLOUTS` (4) no candidate is accepted at all

A held-out mean over 1–3 samples is noise wearing a number's clothing.
Rejecting on sample size is refused-by-default rather than gambling; the
rejection reason names the count so an operator can see *why* rather than
concluding the loop is broken.

### 4. The scorer is injected, not owned

`SkillOptLoop` takes `scorer: Callable[[str, Rollout], float]`. The loop owns
the acceptance decision and nothing else. A loop that also owned scoring could
be handed a scorer the candidate influenced, and the separation would exist only
in prose.

### 5. Every result carries the numbers behind the verdict, and rejections are kept

`SkillOptResult` always reports `incumbent_score`, `candidate_score`,
`heldout_n` and a prose `reason`, accepted or not, and `history` records
rejections as well as acceptances. An acceptance an operator cannot audit is a
change that happened for unstated reasons; a history of only wins hides the
drift this DDR exists to prevent.

### 6. Failures are weighted above successes in `reflect()`

0.5 for a success, 1.0 for a failure. A success confirms the skill already
covered the case; a failure is the only signal that says where it does not.
`select()` is bounded (default k=3) so a candidate cannot grow without limit
(S2).

## Rejected alternatives

- **Accept on `>=`.** Rejected: see 1. This is the decision.
- **Score on training rollouts** (simpler, no disjoint set to manage). Rejected:
  self-scoring, S7.
- **Accept on a statistical significance test** rather than a flat sample-size
  floor. Rejected for now — a t-test over 4–8 rollouts of a hand-written scorer
  implies a precision the inputs do not have. The flat floor is honest about
  being a floor. Revisit when rollout counts are large enough for the test to
  mean something.
- **Let the loop revert an accepted candidate later** if it underperforms.
  Rejected: that is a second acceptance rule with no held-out set behind it.
  Re-running the loop is the supported path.

## Verification

`aether/tests/test_skillopt.py`, 16 tests, runs in CI's existing pytest job.
The tests that matter are the rejections: tie, regression, undersized held-out
set, and train/held-out overlap.

Mutation-checked before commit: changing `if cand > inc:` to `if cand >= inc:`
in the module fails exactly `test_tie_is_rejected_not_accepted` and
`test_history_records_rejections_too` and nothing else. A test suite that still
passes under that mutation would not be testing the decision this DDR makes.

(pytest is not installed on the build host; the suite was executed locally
through a minimal `raises` shim against the real test file, 16/16 pass. CI
remains ground truth.)
