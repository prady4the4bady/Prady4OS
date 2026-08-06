# DDR-850 — four items brought to spec (#47, #48, #50, #52)

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Corrects DDR-848 and DDR-849. Host-side Python, no kernel surface.

## What went wrong

I implemented four Section 3D items from the **item titles** in
`docs/BUILD_TRACKER.md` rather than the **spec text** in
`docs/AETHER_MASTER_FEATURES.md` §3D. The titles are abbreviations. Re-reading
the source paragraph found four gaps:

| # | spec text | what DDR-848/849 shipped | gap |
|---|---|---|---|
| 50 | "TokenJuice **context compression** (≤80% tokens)" | a token *budget* | compression absent |
| 48 | "skill-update validation gate (**`CAP_SOVEREIGN` always**)" | structural checks only | no authority gate |
| 47 | "**harvest→mine→replay→consolidate**; **pauses active agents**" | a single `consolidate()` | stages unnamed, no pause |
| 52 | "`token_count` **+ `latency_ms`** in audit entries" | tokens + cost | latency absent |

Recorded rather than quietly backfilled. The root cause is worth naming: a
tracker line is a *label for* a requirement, not the requirement. Building from
the label produces something that satisfies the label and can still miss the
thing — and it passes review, because the label is what a reviewer checks
against.

The budget in DDR-849 is **kept**, not replaced. It is genuinely needed and the
refuse-don't-degrade decision stands on its own; it simply was not item #50.

## Decision

### #48 — `CAP_SOVEREIGN` always, and a missing approver is refused

`validate_skill_update` now takes `approver_caps` and raises without
`CAP_SOVEREIGN`. **"Always" means a structurally flawless candidate is refused
too.** A skill file states what an agent may do, so revising it is a sovereign
act however well-formed the revision is; structural validity and authority are
different questions, and this function answers both, because a caller holding
one and not the other would otherwise apply the update.

`approver_caps=None` is **refused**, not treated as authorised. The permissive
reading of a missing argument is the one that turns a forgotten parameter into
an ungated self-rewrite.

### #47 — the four stages are named and callable; sleep refuses over live work

`harvest` → `mine` → `replay` → `consolidate` are separate methods.
`consolidate()` refuses while any agent is marked active, and
`pause_agents()`/`resume_agents()` bracket a pass.

Consolidating under live work rewrites the skill of an agent that is mid-task:
the revision lands between two steps of one action, so **neither the old skill
nor the new one describes what actually ran**, and the trajectory cannot be read
back as a single coherent run. Activity is tracked explicitly rather than
assumed, because "the caller will have stopped them" is true right up until it
is not.

### #50 — context compression to ≤80%, which refuses rather than best-efforts

`compress()` evicts by (priority, age) until the context fits 80% of its
original token count.

- **Pinned segments are never dropped** — the system prompt, skill body, current
  task, safety invariants. These are the segments whose absence changes what the
  agent *is*, not merely what it knows.
- **If the target cannot be met without dropping one, it RAISES.** A best-effort
  context is indistinguishable from a successful one at the call site, and the
  agent proceeds believing it has information it does not have. Same decision as
  the budget, for the same reason.
- **Deterministic.** The same context compresses the same way every time. A
  compressor whose output depends on dict ordering makes a bad answer impossible
  to reproduce, and an irreproducible bad answer gets retried rather than fixed.

One token estimator project-wide. Two would let a context pass the compressor
and fail the budget, and the disagreement would read as a flaky bug rather than
a definition problem.

### #52 — `latency_ms` alongside `token_count`

Recorded per call, summed and averaged per agent and per model. Cost alone
cannot distinguish "cheap and useless" from "cheap and fast", which is the
comparison the number exists to support. Negative latency is refused.

## Verification

`aether/tests/test_spec_corrections.py` — 22 tests. Full tree: **108 tests**
across four suites (16 + 36 + 34 + 22), all green.

**Seven mutations, seven kills:**

| mutation | tests killed |
|---|---|
| `CAP_SOVEREIGN` gate removed | 3 |
| missing approver treated as authorised | 2 |
| sleep no longer refuses over active agents | 1 |
| pinned segments become evictable | 3 |
| impossible target returns best-effort | 1 |
| eviction ignores priority | 1 |
| latency not accumulated | 2 |

`test_skillopt_guards.py` wraps `validate_skill_update` with a sovereign
approver rather than threading the argument through 36 call sites: those tests
vary the *candidate* and hold authority constant, while
`test_spec_corrections.py` varies the *approver* and holds the candidate
flawless. Two questions, tested separately.

(pytest is absent on the build host; suites ran through a minimal `raises` shim
against the real test files. GitHub Actions remains in the outage recorded under
DDR-848, so this is **locally verified, not CI-confirmed**, and is committed
without being pushed per the operator's instruction to hold until the outage
clears.)
