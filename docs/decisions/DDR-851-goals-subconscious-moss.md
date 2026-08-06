# DDR-851 — goals.md, subconscious loop, MOSS pipeline

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #53 (`goals.md`), #54 (subconscious loop), #55 (MOSS
source-rewriting pipeline). Host-side Python, no kernel surface.

Built from the spec text in `docs/AETHER_MASTER_FEATURES.md` §3D, per the
lesson recorded in DDR-850 — every clause quoted below is a requirement, not a
paraphrase.

## #53 `goals.md` per agent

**A goal with no checkable success criterion is refused.** "Improve file
handling" cannot be satisfied or failed; it can only be asserted about. Once a
goal is unfalsifiable, every subsequent report against it is unfalsifiable too,
and the tracking apparatus produces confident statements carrying no
information. The criterion need not be numeric — it must be something a third
party could check without asking the agent.

**`CAP_SOVEREIGN` to parse or revise.** Same class of document as `skill.md`:
this is the objective function. An agent that can edit what it is trying to
achieve does not have an objective, it has a preference it can update when the
current one proves inconvenient — and it can satisfy any goal by replacing it
with one already met.

**An agent cannot mark its own goal complete.** `complete()` requires an
attestor. S4 in the place it would be least visible: a self-marked completion
looks exactly like a real one in the record.

**A malformed goal line is rejected, not skipped.** Skipping means a typo
silently removes a goal, and the agent then reports full completion of a list
quietly shorter than the operator's. Same for an empty file — almost always a
format error rather than an agent with nothing to do, and treating it as the
latter makes the mistake invisible.

`MAX_ACTIVE_GOALS = 8`: an agent pursuing more is pursuing none of them. A
forcing function for prioritisation, not a storage limit (S2). Completed goals
do not count against it.

## #54 subconscious loop

Spec: *"wakes every N ticks, goal-diff prompts, bounded by the 60 syscall/s
limiter"* + F#71 *"fires proactive prompts to **idle** agents"*. Each clause is
enforced:

- **Period, not continuous.** A background loop with no period is a busy loop
  competing with the foreground work it exists to support.
- **Goal-diff.** Every prompt carries the goal that justifies it. A prompt with
  no goal behind it cannot be reviewed — an operator asking "why is it
  suggesting this?" would have nothing to read.
- **A MINORITY share of the syscall limit** (25% of 60/s). Stayed under by
  construction, not discovered by being throttled. Background work that consumes
  the budget starves the foreground agent, and the symptom is that *the agent*
  looks slow, not that the subconscious looks wrong. Exceeding it **raises**
  rather than emitting a partial batch: a truncated batch silently drops the
  lowest-priority goals while looking like it kept up. The 1-second window
  slides, so the loop is throttled rather than permanently wedged.
- **Idle agents only.** Prompting a busy agent interrupts work already in
  progress, on the strength of a goal it may already be pursuing.

**The loop has no execution path.** It emits prompts; every one goes through the
ordinary approval path. A background loop that can act is an unsupervised agent
running on a timer.

## #55 MOSS pipeline

Spec: *"staging sandbox, regression gate, SFS snapshot rollback"*; §3C makes
`ACTION_REWRITE_AGENT_CODE` force-PENDING with `CAP_REWRITE` + `CAP_SOVEREIGN`
co-approval and *"regression suite must pass first"*.

    propose → stage → regress → snapshot → promote
                        │                      │
                        └── fail → discard     └── fail → rollback

**A regression suite that did not RUN is not a pass.** This is the guard that
matters most and the easiest to get wrong: `if failures == 0` is *also true when
zero tests ran*, which turns a broken harness into a green light. `ran` and
`total > 0` are checked separately from `failures == 0`, so "no result" is a
distinct state from "passed".

**Never applied in place.** In-place edit means a failed regression leaves
broken source with nothing to restore from — and the agent that would perform
the restore is the one running the broken source. Regression must run against
the *staged* candidate; running it against the live tree tests the code that is
already there.

**Co-approval is two DIFFERENT principals.** One principal holding both
capabilities is one decision wearing two hats, which is exactly what a two-key
rule exists to prevent. The author cannot approve its own rewrite (S4), and one
principal cannot approve twice.

**Snapshot before promote, always.** The state machine makes a promote without a
snapshot behind it structurally impossible rather than merely discouraged: a
rollback path that was never created is discovered at the moment it is needed,
which is the moment it cannot be created.

A no-op rewrite is refused — it still consumes an approval and lands in the
audit log as a change that was made.

## Verification

`aether/tests/test_goals_moss.py` — 36 tests. **Eleven mutations, eleven kills:**

| mutation | tests killed |
|---|---|
| `passed` ignores whether the suite ran | 1 |
| co-approval accepts one principal | 1 |
| author may approve its own rewrite | 1 |
| promote without a snapshot | 1 |
| goals sovereign gate removed | 1 |
| malformed goal line skipped | 2 |
| empty success criterion accepted | 1 |
| completion without an attestor | 1 |
| subconscious truncates instead of raising | 1 |
| subconscious prompts busy agents | 1 |
| subconscious takes the full syscall limit | 2 |

Tree restored and re-run green (36/36) after each.

Section 3D is now **11 of 21**.

(Locally verified, not CI-confirmed — GitHub Actions outage `qcvjkzcs7j74`
remains open. Committed without pushing per the operator's instruction.)
