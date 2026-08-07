# DDR-856 — vector KG, population tournament, run visualiser (Section 3D complete)

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Section 3D #62, #64, #65 — the last three. **Section 3D is now
21 of 21.** Host-side Python, no kernel surface.

Per DDR-855, each was checked against existing code before anything was
written. `graph_session_primer()` was run; the Python host layer is indexed as
"other" by the C-oriented graph, so `agents/memory/knowledge_consolidation.py`
(D-11) and `agents/federation/distributed_experiments.py` (C-02) were read
directly. Neither overlaps: D-11 resolves *conflicting claims* about one key,
C-02 is *quorum voting* among peers. This slice is similarity search and
head-to-head competition. Different questions, so they compose.

## #62 vector knowledge graph — placed in `agents/memory/`, because it is memory

Four refusals carry it:

- **A dimension mismatch is refused, never padded or truncated.** Padding a
  short vector with zeros silently changes its direction, so the nearest
  neighbour returned is *confidently wrong* rather than absent.
- **A zero vector is refused, and `cosine` raises rather than returning 0.0.**
  0.0 is a real similarity meaning orthogonal; using it for "undefined" makes an
  error indistinguishable from a result.
- **Adding to a full graph raises; it does not evict.** The D-13 principle
  applies here too — silent forgetting turns a store into something that cannot
  be reasoned about. The message points at consolidation, which is the
  operator's decision, not a side effect of an insert.
- **An edge needs both endpoints.** A dangling edge is a relation to something
  that is not there, which every traversal must special-case or trip over.

`reinforce()` is the online-learning property the spec names: it moves a stored
embedding toward new evidence incrementally, no reindex. Rate is bounded to
(0, 1] — 0 is a silent no-op that looks like an update, above 1 overshoots past
the new evidence to somewhere neither observation supports. A reinforcement that
cancels an embedding to zero is refused rather than leaving a node no query can
match.

## #64 population tournament — competition, not consensus

- **A variant that never played cannot win.** Below `MIN_MATCHES` it is
  *unranked*, not ranked last. "Untested" and "tested and bad" are different
  states, and collapsing them lets a variant win by having avoided scrutiny.
  Unranked entries are returned carrying the reason, not filtered out — dropping
  them makes a variant that never played indistinguishable from one never
  entered.
- **A tie does not promote.** Same bar as DDR-847: promoting on a draw changes
  the incumbent with no evidence anything improved. A drawn match has *no*
  winner — never index order, which is proposer-controlled.
- **Both scores are retained.** A rank from win counts alone cannot tell a
  variant that barely lost every game from one that was crushed, and those call
  for different next moves.
- A population of one is refused: it always produces a winner and never produces
  information.

## #65 replayable run visualiser

Reads the **existing** JSONL trajectory (DDR-849) through the writer's own
`read_trajectory`. A second parser would drift from the writer, and the first
symptom would be a run rendering subtly wrong rather than failing.

- **It never un-redacts.** Secrets are scrubbed at write time; a viewer reaching
  for the raw log to show "what really happened" would undo that on a page
  someone screenshots.
- **Deterministic.** Same trajectory, byte-identical HTML. "Replayable" is the
  requirement; a page carrying a render timestamp is not replayable, and diffing
  two runs stops working the moment output varies for reasons the runs did not.
- **Self-contained.** No fonts, scripts, images or external CSS. On an offline
  OS a fetching viewer breaks exactly when it is needed, and under privacy mode
  it is also an egress attempt.
- Everything is HTML-escaped. Agent output routinely contains markup; unescaped,
  a trajectory record is a stored-XSS vector in a page built from untrusted
  content.

## The mutation harness caught two real gaps — and one in itself

Run through `tools/mutation/mutate.py` (DDR-853). It reported **15/15 killed**,
then a closer read showed two entries claiming "killed by 1" against a suite
that had passed 35/35 — a contradiction.

**Cause: the harness matched any line starting with `FAIL`,** and the test
runner's own summary line reads `FAILREG OK -- 35 passed`. A *passing* run was
being counted as a kill. That is the third appearance of this project's
recurring defect inside the tooling built to detect it (after DDR-853's skipped
targets and stale bytecode), and it is why the kill pattern is now anchored to
`^  FAIL  <name>$`, with a non-zero/non-one runner exit treated as untrustworthy
output rather than an empty kill list.

With detection fixed, **two mutations genuinely survived**:

| mutation | why it survived | fix |
|---|---|---|
| M7 — `nearest` tie-break removed | `nodes` sorted by key *and* `nearest` broke ties by key. With a pre-sorted input and a stable sort the tie-break was **unreachable** — dead code the project forbids | `nodes` now returns insertion order; the ordering guarantee lives in exactly one place, so the code providing determinism is code a test can kill |
| M15 — event summary sort removed | the test compared dicts, and **dict equality ignores order** | added `assert list(...) == [...]`; the sort was previously unverified |

Both are worth recording rather than quietly fixed. M7 is the more instructive:
two mechanisms implementing one guarantee meant neither was tested, and it
looked like defence in depth.

## Verification

`aether/tests/test_kg_tournament_visualiser.py` — **36 tests**, 15/15 mutations
killed after the two gaps were closed.

**Section 3D: 21 of 21 complete** (#45–#65).

(Locally verified, not CI-confirmed. Run `31127913346` on `4bdbf3a` was still
in progress at commit time — the first run Actions has scheduled since outage
`qcvjkzcs7j74` began throttling webhooks.)
