# DDR-1071 — Five Group E rows present shipped, CI-registered work as remaining

**Status:** DOCS-ONLY correction + an assessment that deliberately does not build a checker
**Date:** 2026-09-06
**Branch:** `dev/phase1-seyp3n`
**Class:** DDR-1063 §7b (a stale row presenting built work as remaining) — **five instances in one table**, a scale not previously seen.

---

## 1. What was measured

Every gate name in CLAUDE.md's **GROUP E** table, checked against
`Makefile` and `tools/ci/gate_shards.txt` rather than inferred from any DDR —
the discipline DDR-1007 recorded for `smoke-wmmax` (*"Verified in the Makefile,
not inferred from the DDR"*).

**All twelve exist.** Five of them sit on rows that carry **no completion
marker at all** — no `~~strikethrough~~`, no ✅ — and are therefore read as
remaining work:

| row | gate | Makefile | shard | tier |
|---|---|---|---|---|
| PS/2 modifier keys | `smoke-modkeys` | :3094 | 7 | **strict** |
| Super+M physical binding | `smoke-superkey` | :3079 | 8 | **strict** |
| Alt-Tab with modifier plumbing | `smoke-alttab` | :4083 | 9 | fast |
| Per-window restore from dock | `smoke-perrestore` | :3420 | 4 | fast |
| OKLab horizon bands / animated mesh | `smoke-horizon` | :3925 | 2 | fast |

**Existence is not the claim — registration is.** None of the five is in the CI
exclude list, so all five run on **every** CI suite. Two are **strict** tier.
This is not "a target that happens to be in the Makefile"; it is coverage the
project has been getting on every run while its own backlog said the work was
not done.

## 2. Four of the five are complete against what their row asks

Checked against the row's own wording, not against the gate's name:

- **`smoke-modkeys`** injects `a f1 up ctrl-c b` through IRQ1 to NSI 46 + 96 —
  an F-key, an arrow and a Ctrl chord, i.e. exactly the row's *"F-keys, arrows,
  Alt, Ctrl, Meta/Super"*. **And it asserts rather than merely injects**
  (checked, because the distinction is the whole of DDR-1070): `MODKEYS FAIL`
  is fatal, `PRADYOS_MODKEYS_OK` is required, and `PRADYOS_MODKEYS_PAIR_OK` is
  required — DDR-993's paired-modifier / make-break identity kernel arm.
- **`smoke-superkey`** injects `meta_l-m` twice, which is the row's toggle.
- **`smoke-alttab`** covers Alt+Tab cycling *and* plain-Tab-to-focus, and
  **exceeds** its row (*"Upgrade from plain Tab"*). Its own header records why it
  takes TWO SEQUENTIAL BOOTS: `input_inject.sh` replays its key list four times,
  so in a single boot a plain Tab always precedes the next Alt+Tab and the
  ordering carries no information — the same vacuity reasoning DDR-1027 arm E
  and DDR-1068 M1 arrived at independently.
- **`smoke-perrestore`** minimizes BETA **and** ALPHA then restores **only**
  BETA from its tile — precisely the row's *"DDR-717 restores all; add
  per-tile"*, and the two-window form is what stops the assertion being vacuous
  (DDR-1008's own reasoning).

## 3. The fifth row is HALF right, and is corrected rather than closed

`OKLab horizon bands / animated mesh` names **two** things. `smoke-horizon`
asserts `PRADYOS_HORIZON DAWN`, `PRADYOS_HORIZON DUSK` and
`PRADYOS_HORIZON_OK`, so **the bands are built and gated** (DDR-1012, which also
made the gate measure PIXELS because a sentinel-only check passes on a mutant
that draws nothing). **The animated mesh is not built** — DDR-1012 assessed and
deferred it with a reason.

So this row is corrected, not struck through. Marking it done would be the
mirror of the defect this DDR reports.

## 4. Why these five and not the others

The rows that *were* corrected — Ctrl+Alt+T, `smoke-wmmax`, `smoke-resizeall`,
`smoke-surfclose`, `smoke-sharedpte`, `smoke-vdso` — were each corrected **by
the session that happened to work on them**. The five here are the ones nobody
revisited after the work landed.

That is the structural shape, and it is worth carrying: **a backlog row is
updated when someone touches it, and never swept.** Correction is a side effect
of doing adjacent work rather than a process, so a row whose work finished
cleanly and drew no follow-up is exactly the row that stays stale. The DDR-1063
§7b class is not a series of individual oversights; it is what this update
discipline produces by default.

## 5. A checker is ASSESSED and deliberately NOT built

The mechanical rule is easy to state: *for every backlog row whose named gate
exists in the Makefile **and** is registered in `gate_shards.txt`, the row must
carry a completion marker.* It would have caught all five.

**It is not built, for a measured reason.** `smoke-horizon` is the
counterexample sitting in this very DDR: a gate can exist and be registered
while the row's feature is **legitimately half done**. A checker with that rule
reddens on §3's row, which is *correct in-progress state* — and DDR-1063 set the
criterion explicitly when it built `ci-docstate-check`: *"a currency check would
redden on correct in-progress work and get removed."* A check that fires on
correct state does not survive; it gets an exemption list, and an exemption list
is the staleness it was built to prevent, one level up.

The distinguishing signal would have to be **semantic** — does the gate cover
everything the row's prose asks for? — and nothing in the tree can read that.

**Recorded as the buildable narrower variant, not built here:** a check that a
row naming a *registered, strict-tier* gate carries some marker would have caught
`smoke-modkeys` and `smoke-superkey` with no false positive available today —
but it rests on a tier convention that is not a promise, so it would be a rule
about today's shard file rather than about the claim. Revisit if this class
recurs after the release.

## 6. What this changes

Group E now reads as: **everything shipped and gated except the animated mesh
(deferred, DDR-1012) and OPEN-1**, which is a materially different picture from
a table showing five open rows days from a deadline. The gate coverage did not
change — only the record of it.

## 7. NOT claimed

- **No code changes.** `kernel.bin` is untouched; this is a documentation
  correction and `git diff --stat` shows only Markdown.
- **The five gates were not re-run here.** What was measured is that they exist,
  are registered, are not excluded, and — for `smoke-modkeys`, read in full —
  assert real sentinels in both directions. Their green status comes from CI
  having run them, not from a run in this session.
- **`smoke-horizon`'s row is not closed** (§3), and the animated mesh remains
  deferred with DDR-1012's reason.
- **OPEN-1 is untouched.** The `smoke-surfdestroy` row is unaffected by any of
  this and remains the one genuinely open Group E item.
- **No new gate; 177 unchanged.** No checker was built (§5).

---

*DDR-1071. Cross-refs: DDR-1063 §7b/§7c (the class, and why a currency check is
refused), DDR-1007 (verify in the Makefile, not from the DDR), DDR-1012
(horizon bands built, mesh deferred), DDR-1008 (per-window restore), DDR-993/991
(modifier arms), DDR-995 (Alt+Tab), DDR-992 (Super+M), DDR-1070 (assert, do not
merely exercise).*
