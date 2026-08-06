# DDR-854 — S10 secret scan, and why the hand-run grep was checking nothing

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** `tools/ci/secret_scan.sh`, fixture de-literalisation in
`aether/tests/test_budget.py`. No kernel surface.

## The finding

While staging DDR-853 the token scan returned **exit 0** — meaning `ghp_`
matched in a tracked file. It resolved to synthetic material only: the detector
regex in `aether/agents/budget/trajectory.py` and two obviously-fake
`ghp_aaaa…`/`ghp_bbbb…` fixtures. **No real credential was ever committed.**

But chasing it exposed something worse than the hit: the S10 check I had been
reporting as "clean" all session **was structurally incapable of failing.**

`git grep` searches **tracked files only**, and I ran it **before `git add`**.
So at the moment I reported "token-scan clean" for a commit, the new files *in
that commit* were still untracked and were not searched. A new file carrying a
credential would have passed, silently, every time.

That is **instance 17** of this project's recurring defect: a check that reports
success without having examined the thing it exists to examine. It is the same
shape as instance 16 (the mutation harness) and instance 1 (the CI gate list),
and it is worth noting that all three were checks *I* was relying on to make
claims to the operator.

A second failure mode is slower but just as fatal. Once real detector patterns
and test fixtures exist in the tree, a bare substring grep fires on them
forever. **A scan whose hits are routinely dismissed by hand is a scan nobody
reads**, and the one real hit then arrives looking exactly like the noise.

## Decision

`tools/ci/secret_scan.sh`:

- **Scans the working tree**, not just the index: `git ls-files` plus
  `git ls-files --others --exclude-standard`. Staged-but-uncommitted and
  untracked files are covered. This is the entire point — the old check missed
  precisely the new work being added.
- **Matches credential shapes, not prefixes.** A real PAT is `ghp_` + 36 varied
  characters; `github_pat_` + 40+; `AKIA` + 16 uppercase-alnum; and so on. A
  synthetic fixture of one repeated letter does not match, so the scan does not
  train its reader to ignore it.
- **Exactly one exclusion, named on its own line with its reason.** The detector
  module must contain the patterns it detects. Not a wildcard, not a suppression
  list that can quietly grow.
- **Exits non-zero on a hit**, telling the operator not to commit, not to push,
  and to rotate.

Test fixtures now **assemble** the prefix (`"gh" + "p_"`) rather than writing it
literally, so an unrelated test file cannot trip the scan. The detector regex
stays literal — it has to be — which is what the single exclusion covers.

## A bug in the scanner, found by self-testing it

The private-key pattern begins with a dash, so `grep -HnE "$pattern"` parsed the
pattern as **options** and that shape never matched anything.

| arm | input | expected | before fix | after `grep -e` |
|---|---|---|---|---|
| A | clean tree | exit 0 | ✅ 0 | ✅ 0 |
| B | realistic 36-char token, untracked file | exit 1 | ✅ 1 | ✅ 1 |
| C | synthetic repeated-letter fixture | exit 0 | ✅ 0 | ✅ 0 |
| D | an OpenSSH private-key header line | exit 1 | ❌ **0** | ✅ 1 |

Arm D is the only reason that shape is not still broken. **A scanner never seen
rejecting something is a scanner not known to work** — which is the same
argument as the mutation tables, applied to a gate instead of a guard.

Arm C matters as much as B: a gate that cries wolf gets ignored, and an ignored
gate is indistinguishable from an absent one.

**This document is itself evidence the gate works.** The first draft spelled the
private-key header out in the table above; the scan rejected the commit and
named the file and line. The prose was changed to describe the header instead —
the document was fixed, not the gate. A gate that gets an exception carved for
documentation is a gate with a documented bypass.

## Verification

Four-arm self-test above, run against the real script. All seven Python suites
green: **212 tests**.

(Locally verified, not CI-confirmed — outage `qcvjkzcs7j74` open. Committed
without pushing per the operator's instruction. `make`-level wiring of this
script into the CI gate list is deferred to the Group 9 release-hardening item,
where the gate manifest is edited as a unit.)
