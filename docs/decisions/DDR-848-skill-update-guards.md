# DDR-848 — guards on skill self-modification: sleep, validation, transfer

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #47 (SkillOpt-Sleep), #48 (skill-update validation),
#49 (multi-agent transfer). Host-side Python (`aether/`), no kernel surface, no
new syscall. Builds directly on DDR-847.

## Context

DDR-847 gave SkillOpt the ability to rewrite an agent's skill file on evidence.
That capability and the document it edits are in tension: **a skill file says
what the agent may do, and the loop's optimisation pressure points straight at
the refusals.** Every refusal is a task the agent declines and therefore scores
zero on. Removing one raises the score. Nothing in DDR-847 prevents that — its
acceptance rule asks only whether the candidate scored better, not whether the
candidate is a legitimate skill file.

These three items are the bound on what "improve" is allowed to mean.

## Decision

### #48 — a candidate is validated BEFORE it is scored

Not after, and not "the scorer will catch it". A candidate that escalates
capability must never reach a comparison that a good score could carry it
through. `SkillOptSleep.consolidate()` therefore builds the candidate, validates
it, and only then evaluates.

Three rules carry the weight:

1. **Capabilities may only shrink.** The declared set must be a subset of what
   the kernel granted. A revision that writes itself a new capability is
   privilege escalation through prose (S1). `granted` defaults to the
   incumbent's own declared set, so the common two-argument call means "may not
   gain anything it did not already have" — a caller who forgets the argument
   gets the strict behaviour, not the permissive one.
2. **Refusals may not be thinned.** The count may rise and may not fall. This is
   the edit the optimiser is most rewarded for making and the one it must never
   be allowed to make. Stated as "may not decrease" rather than only "at least
   3", which is strictly stronger: an agent starting with six refusals cannot
   quietly settle at three.
3. **Required structure survives**, including `## Invariants`. An agent that
   edits away its own binding to S1–S8 has deleted the text that told it what it
   was bound by.

**Capabilities are parsed from the declaration line, not the whole document.**
This is the subtle one. A whole-document scan would read BASE's refusal —
*"refuses to write to a `CAP_SOVEREIGN`-locked path"* — as a claim to *hold*
`CAP_SOVEREIGN`. A gate that miscounts refusals as grants punishes exactly the
files that are most careful about naming what they decline.

The token estimator is deliberately the *same* approximation
`test_agent_skills.py` uses. Two estimators policing one budget means a file can
pass one gate and fail the other, and the disagreement would read as a flaky
test rather than a definition problem.

### #47 — sleep uses the same acceptance bar as waking

There is **no relaxed offline threshold**. No "we have more data now, so a tie
is good enough". An offline pass is a better opportunity to be rigorous, not a
licence to be lenient — and a second, weaker acceptance path would rapidly
become the path every change flowed through, at which point DDR-847's guarantees
would still be in the code and no longer true of the system.

Two supporting properties:

- **The train/held-out split is deterministic and arrival-order independent**,
  keyed on `sha256(salt:task_id)`. Splitting by insertion order correlates the
  held-out set with time, so a skill that happened to improve mid-session scores
  well for reasons unrelated to the edit. Splitting randomly makes a rejection
  impossible to reproduce — and an unreproducible rejection gets re-run until it
  passes, which is the same as having no gate.
- **Recording never revises.** Rewriting a skill mid-task changes the thing being
  measured while it is being measured.

The journal is bounded (4096) and drops the **oldest** entry, counting drops.
An unbounded rollout log is a memory leak that only manifests on long-lived
agents — the ones that matter (S2). The count is surfaced because a
consolidation over an overflowed journal saw less than the agent actually did,
and the operator reading the result needs to know that.

### #49 — a transfer is a proposal, never an application

A lesson that improved KRYOS is *evidence about KRYOS*. It is a *hypothesis*
about PRAX, and the only thing that can settle it is PRAX's own held-out
evaluation. So `propose_transfer` returns proposals for the recipient's journal
and applies nothing.

Splicing a lesson directly into the recipient's skill would create a second path
into a skill file that bypasses held-out scoring entirely — and being the easy
path, it would become the path everything took.

Supporting rules: a lesson naming a capability the recipient lacks is **not**
transferred (teaching a technique the kernel denies does not make an agent more
capable, it makes it fail later and further from the cause); transferred lessons
are discounted to 0.5 weight, because second-hand evidence is weaker evidence
and at full weight one agent's experience can outvote the recipient's own on the
recipient's own selection step; and provenance is carried, with the task_id
namespaced (`PRAX:t1`) so a transferred lesson can never collide with or
masquerade as a task the recipient actually ran.

## Rejected alternatives

- **Validate after scoring**, treating validation as a final safety net.
  Rejected: it makes an escalating candidate's score meaningful, and a
  high-scoring rejected candidate is an argument for relaxing the gate.
- **Let the scorer penalise bad candidates** instead of a hard gate. Rejected:
  a penalty is a price. Escalation is not something an agent should be able to
  buy with performance elsewhere.
- **Random train/held-out split.** Rejected: unreproducible rejections.
- **Apply transfers directly** when the source and recipient share a role.
  Rejected: see #49. "Similar agents" is exactly the argument that would be made
  for every transfer.
- **Enforce only `>= MIN_REFUSALS`** rather than monotonic non-decrease.
  Rejected: permits a slow slide down to the floor, and each individual step
  passes.

## Verification

`aether/tests/test_skillopt_guards.py` — 36 tests, all of them refusals, in
CI's existing pytest job. Combined with DDR-847's 16, the SkillOpt family is
52 tests.

**Mutation-checked before commit — every guard is killed by at least one test:**

| mutation | tests killed |
|---|---|
| capability-escalation check disabled | `..cannot_grant_itself_a_new_capability`, `..granted_set_overrides..`, `..validation_before_scoring` |
| refusal-thinning check disabled | `..cannot_thin_its_refusals` |
| capabilities parsed from whole document | 7, incl. `..refusal_named_in_prose_is_not_read_as_a_grant` |
| sleep skips validation | `..consolidate_runs_validation_before_scoring` |
| split keyed on arrival order | `..split_is_deterministic_and_order_independent`, `..salt_changes_the_split` |

A guard no test kills is a guard that is not being verified; that table is the
evidence that none of these five are in that state. Tree restored and re-run
green (36/36) after each mutation.

(pytest is absent on the build host; both suites ran through a minimal `raises`
shim against the real test files. CI's pytest job remains ground truth, and at
time of commit GitHub Actions is in a major outage — see `docs/build_status.md`.)
