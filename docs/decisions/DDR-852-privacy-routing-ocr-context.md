# DDR-852 — privacy mode (ring-3), model routing, OCR→memory, multi-modal context

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #56 (OCR→memory), #57 (multi-modal context builder),
#58 (privacy mode, ring-3), #59 (model routing). Host-side Python, no kernel
surface. Built from the §3D spec text.

## #58 — privacy mode, and what this layer is NOT

The kernel already enforces this. `kernel/aether/` blocks non-local egress in
privacy mode and records `AR_PRIVACY_BLOCKED` with its own audit code (DDR-802,
gate `smoke-privacy-netfilter`). **What ring-3 adds is an early, readable
refusal** — an agent routing to a remote model in privacy mode should be told
why by the router, not discover it as an opaque `-EPERM` three layers down.

Stating that plainly is part of the decision. A ring-3 check that *looks* like
enforcement invites someone later to weaken the kernel one as redundant. It is
not redundant; it is the only one that counts. This layer is a convenience that
must never be trusted.

**It fails closed.** `PrivacyState.UNKNOWN` blocks remote routing. Assuming
"off" when the state cannot be read turns an unreadable config into an open
egress path, and nothing in the logs would say so.

**`PrivacyBlocked` is its own exception type**, not a flag on `RoutingError`.
Folding it in makes "did privacy mode actually stop anything?" unanswerable —
the same reasoning that gave `AR_PRIVACY_BLOCKED` a distinct audit code rather
than a bit on a generic denial.

**An unresolved hostname is not local.** `is_local_host` classifies literal
addresses (loopback, RFC1918, link-local) and returns False for every name,
including names that read as local. Resolution happens elsewhere and can change
between the check and the connection; treating a name as local would make the
classification depend on a DNS answer this function never saw. Per the standing
rule, no test endpoint is written as a literal credential — hosts are
classified, not pattern-matched.

## #59 — model routing

Refuses rather than returning a near-miss. A caller handed a model whose context
window does not fit would discover it as a truncation, which is the failure this
project refuses everywhere else. The rejection message names *each* model and
why it was excluded, so "no model satisfies the request" is diagnosable.

Deterministic: candidates are sorted by name before filtering and the winner is
chosen by `(cost, -quality, name)`. A router whose choice depends on dict
insertion order makes a bad answer impossible to attribute to a model.

Cost and quality are **required** fields on `ModelSpec`. A model with no
declared cost cannot be compared against a budget, and defaulting it to zero
would make the cheapest option always be the one nobody has measured — the same
defect as DDR-849's `rates.get(model, 0.0)`.

## #56 — OCR→memory

**OCR is lossy in a way that reads as authoritative.** A misrecognised digit
does not arrive flagged as uncertain; it arrives as a number, is stored as a
fact, and is later retrieved with the same confidence as something the operator
typed. By the time it is wrong, nothing upstream records it was ever a guess.

So: **confidence travels with the text, and anything below the floor (0.80) is
QUARANTINED** — retrievable and clearly marked, not discarded (the operator may
want it) and not silently promoted. `ingest()` returns quarantined records too,
so a caller cannot read the stored set as "nothing was found there".

Provenance is mandatory: a record whose source cannot be named cannot be
re-checked against the document when it is doubted. Duplicate regions (common in
multi-column OCR) are suppressed, because storing a line twice makes repetition
look like corroboration.

## #57 — multi-modal context builder

**A fragment with no measurable token cost is refused.** Defaulting it to zero
would make unmeasured content free — and free content is never evicted, so the
one fragment nobody could size would survive every compression.

**A quarantined memory record enters the context marked `[UNVERIFIED OCR]` and
at the lowest priority.** Passing it in clean would launder a low-confidence OCR
guess into an authoritative-looking line of context — the #56 failure re-entering
through a different door.

Modality priorities order eviction: task text (5) > memory (3) > OCR (2) >
scene (1). A scene description is re-derivable from the next frame; the
operator's own words are not.

## Verification

`aether/tests/test_routing_ocr.py` — 36 tests. **Ten mutations, ten kills:**

| mutation | tests killed |
|---|---|
| UNKNOWN privacy fails open | 2 |
| privacy check dropped from routing | 3 |
| unresolved hostnames treated as local | 4 |
| router returns a near-miss | 3 |
| tie-break depends on registration order | 2 |
| low-confidence OCR stored as fact | 3 |
| quarantined memory enters unmarked | 1 |
| empty fragment sized at zero | 1 |
| duplicate suppression removed | 1 |
| modality priorities flattened | 1 |

Section 3D is now **15 of 21**.

(Locally verified, not CI-confirmed — outage `qcvjkzcs7j74` open. Committed
without pushing per the operator's instruction.)
