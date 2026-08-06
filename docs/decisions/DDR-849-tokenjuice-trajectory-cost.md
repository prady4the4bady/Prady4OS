# DDR-849 — TokenJuice, trajectory log, cost accounting

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #50 (TokenJuice), #51 (JSONL trajectory), #52 (cost
accounting). Host-side Python (`aether/agents/budget/`), no kernel surface, no
new syscall.

## Context

An agent that spends resources needs three things that are easy to get subtly
wrong in the same direction: a ceiling, a record, and an attribution. Each has
an obvious implementation that fails *quietly* — the failure looks like success
and the number an operator reads stays plausible.

## Decision

### #50 — a budget REFUSES; it never degrades

When a request exceeds the ceiling, `BudgetExhausted` is raised. The budget
never shrinks the request to fit.

A soft budget is worse than no budget. The run keeps going and quietly does less
per step, so it "succeeds" having answered from a truncated context, and the
only evidence is that the output got vaguer. A refusal is a fact an operator can
act on; a silently truncated context is a wrong answer wearing a right answer's
shape.

- **Reserve (5%).** A budget spent to exactly zero leaves an agent unable to
  finish — no summary, no cleanup, no audit entry saying why it stopped.
  Ordinary work can never reach the reserve; completion work can.
- **Grants are debited at grant time, not at child spend.** A parent whose
  available figure still counts tokens it has already promised away will
  authorise work it cannot pay for, and the shortfall surfaces at the child, far
  from the decision that caused it. A parent that can hand a child more than it
  holds has escalated by delegation (S1).
- **Two-phase reserve/commit.** Charging after the fact means the overspend has
  already happened, and "we noticed afterwards" is not a ceiling. A commit that
  exceeds its reservation raises *and puts the reservation back*, so a failed
  commit does not also corrupt the books.

**A bug found in this module before it shipped, recorded because the class of it
matters:** the first draft tracked one `_outstanding` counter for both pools, so
a completion reservation was debited against the fenced reserve *and* against
ordinary availability. Every individual operation looked correct. Normal and
completion are now tracked separately.

**And a second, worse one — in the check meant to catch the first.**
`check_invariant()` originally asserted
`total == spent + outstanding + granted + available + reserve_remaining`. That
is a **tautology**: `available` is *derived* by subtracting the others from
`total`, so the assertion could never fail. It read as a strong invariant and
was decorative. It now asserts the independently-tracked quantities — the
counters versus the open reservations, and non-negativity of each pool — and the
tautology is documented as something deliberately *not* asserted. A check that
cannot fail is indistinguishable from no check, which is this project's
recurring defect wearing a reassuring costume, this time in code written to
guard against it.

### #52 — an unknown model RAISES; it is never priced at zero

`rates.get(model, 0.0)` is the bug this decision exists to prevent. It keeps the
code short and nothing ever crashes — and it makes the cost of every model
nobody has priced yet *invisible*. The total stays plausible, the report looks
complete, and the one number an operator uses to decide whether an agent is
worth running is wrong in the direction that says "keep going".

Zero is a legitimate price (a local model). The point is that it must be
**declared**, so "free" and "unknown" are distinguishable.

Supporting rules: pricing happens **before** the ledger is touched, so a failed
pricing call cannot still move the token totals; costs are **integer
micro-cents**, because float money produces a total that disagrees with the sum
of its own line items by an amount small enough to be dismissed as rounding for
a long time; rounding is **up**, because the direction of an error in a spend
figure should never be the reassuring one; input and output are priced
separately, since collapsing them makes a prompt-heavy and a generation-heavy
agent look identical when they are not.

### #51 — append-only JSONL, redacted on the way IN

JSONL over a single JSON array for one reason: **a run killed mid-write must
still be readable.** A truncated JSON array is not valid JSON at all and takes
the whole run with it; truncated JSONL loses exactly its last line. The reader
tolerates a truncated *tail* and refuses mid-file corruption — a bad line with
good lines after it is not a truncated write, and tolerating it would discard
data while reporting success.

**There is no update, delete, or rewrite path.** A trajectory an agent can edit
says whatever the agent's last revision said, which is worth less than no
record, because a record that can be quietly corrected is one an operator will
trust.

**Redaction happens before the write, not at read time.** Redacting on read
means the secret is on disk and every future reader is one code path away from
printing it. Secret *keys* are matched as case-insensitive substrings
(deliberately over-broad: a false positive costs a log field, a false negative
costs a credential) and secret-*shaped values* are caught regardless of the key
they arrived under. `append()` returns the redacted record, so a caller that
echoes the return value cannot re-leak what was just scrubbed. Recursion is
depth-bounded — a logger that can kill the run it is logging is worse than no
logger.

## Rejected alternatives

- **Soft budget that truncates context to fit.** Rejected: see #50.
- **`rates.get(model, 0.0)`.** Rejected: see #52. This is the decision.
- **Float dollars.** Rejected: unreconcilable totals.
- **Single JSON array trajectory.** Rejected: unreadable if truncated.
- **Redact at read time**, keeping the raw log for debugging. Rejected: that is
  a plaintext credential store with a convention attached.
- **Assert the full accounting identity.** Rejected once discovered to be a
  tautology; see #50.

## Verification

`aether/tests/test_budget.py` — 34 tests, in CI's existing pytest job.

**Mutation-checked before commit; all ten mutations are killed:**

| mutation | tests killed |
|---|---|
| unknown model priced at zero | 2 |
| pricing rounds down | 1 |
| ledger records tokens before pricing | 3 |
| budget truncates instead of refusing | 3 |
| completion reservation charged to ordinary pool | 3 |
| grant debited lazily | 2 |
| failed commit consumes the reservation | 1 |
| trajectory redaction disabled | 4 |
| mid-file corruption tolerated | 1 |
| redaction depth bound removed | 1 |

Tree restored and re-run green (34/34) after each.

(pytest is absent on the build host; the suite ran through a minimal `raises`
shim against the real test file. CI's pytest job remains ground truth, and
GitHub Actions is in a major outage at time of commit — see
`docs/build_status.md`.)
