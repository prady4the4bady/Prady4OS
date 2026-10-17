# DDR-1117 — THE OPEN-2 HUNT WORKFLOW HAS NEVER RUN AND CANNOT BE STARTED

**Status:** assessment, docs-only. **No code change, no gate, no kernel rebuild.**
**NO DEFECT FOUND IN THE WORKFLOW FILE** — the defect is in **where the file lives**.
**NO FIX APPLIED: the remedy is a push to the DEFAULT BRANCH, which is operator-owned.**

---

## §1 — THE FINDING

`.github/workflows/open2-hunt.yml` was built by **DDR-1097** as *operator approach #1*:
an isolated CI job that boots the `OPEN2_HUNT` kernel repeatedly across parallel
runners, with a weekly `cron` that DDR-1097 explicitly identifies as **approach #4**
(*"wider time/hardware sampling, which DDR-1096 §2.4 judged sound with no local form"*).

**It has never run, and no trigger can start it.**

Measured, not inferred:

| measurement | result |
|---|---|
| `actions_list method=list_workflows` | **2 workflows**: `ci.yml` and Dependabot. **`open2-hunt.yml` is not among them.** |
| `GET …/workflows/open2-hunt.yml/runs` | **404 Not Found** |
| `git remote show origin` | default branch = **`dev/phase1`** |
| `git ls-tree origin/dev/phase1 -- …/open2-hunt.yml` | **ABSENT** |
| `git ls-tree origin/main -- …/open2-hunt.yml` | **ABSENT** |
| `git branch -r --contains <its commit>` | **`origin/dev/phase1-seyp3n` and nothing else** |
| added | `709d0e2`, 2026-09-12, DDR-1097 |

**The file itself is correct.** Its `on:` block was parsed rather than eyeballed and
carries **both** real triggers — `workflow_dispatch` (with `lanes`/`runs`/`hunt`
inputs) and `schedule: cron '17 4 * * 0'`. Every one of DDR-1097's stated design
properties holds: no `push`, no `pull_request`, so it can never enter the 3-green
criterion; it registers no `smoke-*` target, so `ci-shard-check` is untouched.

**The mechanism is GitHub's, and it defeats BOTH triggers at once:** a `schedule:`
trigger is honoured **only from the default branch**, and a workflow must exist on the
default branch for `workflow_dispatch` to be exposed at all. A workflow file living
only on a feature branch is therefore **not merely unscheduled — it is unstartable by
any means**, which is exactly what the two-workflow listing and the 404 report.

---

## §2 — THE CLASS, AND IT IS ONE THIS PROJECT HAS ALREADY NAMED

This is **DDR-1043's finding one level up**. That DDR found that `boot_test.sh` had
carried a QMP vCPU-dump watcher *"for a long time"* and `grep -rn QEMU_QMP_DIAG` found
**nothing that set it** — so *"the one instrument that can answer 'the kernel stopped
printing, what was the CPU doing?' was switched off in CI, the only place that failure
has ever been seen."*

Here the instrument is not switched off by a missing variable; it is **unreachable by
construction**, and the symptom is identical: **the dedicated mechanism for hunting
OPEN-2 has produced zero observations while OPEN-2 remains the release's one blocker.**

**And it is a shape no amount of reading the file could catch.** DDR-1097 §"three
defects in my own workflow file, fixed before it ever ran" found a decorative `lanes`
input, a `tee` into a directory created too late, and an exit code swallowed by a
pipeline — all **inside** the file, all found by reasoning about its contents. This
fourth one is **not in the file**. It is in the file's **location**, which is
invisible to every check that opens it.

---

## §3 — WHAT IS *NOT* AFFECTED. DDR-1097's WORK IS NOT WITHDRAWN

Stated first, because the finding is easy to over-read:

* **DDR-1097's harness is proven and that proof stands.** It was demonstrated
  **end-to-end, locally, in both directions**: the forced build
  (`OPEN2_FORCE_RECYCLE=1`, kernel `9ff5715403f73284`) produced exactly 8 `RECYCLED`
  lines and the campaign scored `signal_runs=1`; the real hunt kernel
  (`e88739745b21238a`) scored **3/3 `signal_runs=0`**, no false positives. None of that
  came from CI and none of it is disturbed.
* **The `tid` re-check, the churn classifier and the build-side false-clean finding are
  all local results** and are untouched.
* **No gate is affected.** The hunt registers no `smoke-*` target; `ci-shard-check`
  reads **179 gates** exactly as before.
* **The release is not blocked by this.** The hunt is an *investigation* tool, not a
  gate — nothing in the 3-green criterion or the ISO depends on it.

**What never happened is the CI-side arm**: the wide time/hardware sampling that was
approaches #1 and #4, i.e. the only part of DDR-1097 that could have produced a *new
artefact*. DDR-1097 was careful to claim no artefact (*"a RECYCLED line has NEVER been
observed outside its forced build"*) — this DDR explains that it could not have been
otherwise.

---

## §4 — THE REMEDY IS OPERATOR-OWNED AND IS **NOT** TAKEN HERE

The workflow becomes startable the moment the file exists on **`dev/phase1`**, the
default branch. Two routes:

1. **Merging PR #17** (`dev/phase1-seyp3n` → `dev/phase1`) carries it there with
   everything else. That is the release path and is the operator's call.
2. **Landing the single file on `dev/phase1` directly**, which starts the hunt without
   waiting on the merge.

**Neither is done here.** Both are pushes to a branch other than
`dev/phase1-seyp3n`, and this session's standing constraint forbids that without
explicit permission — the same disposition as `PRE_LAUNCH_CHECKLIST` §1.3's rule that
merging is the operator's action. **Measured and reported, not merged.**

**One practical note for whoever lands it:** after the file reaches the default branch,
the **first scheduled fire is up to a week out** (`17 4 * * 0`, Sunday 04:17 UTC), so a
manual `workflow_dispatch` — which also only becomes available at that moment — is what
actually starts the hunt.

---

## §5 — THE CHEAP SUBSTITUTE, AND WHY NO CHECKER IS BUILT

A checker is **assessed and refused**, on this project's now-repeated grounds
(DDR-1071 §5, DDR-1072 §2, DDR-1081 §3, DDR-1086 §4, DDR-1107 §3, DDR-1113 §5,
DDR-1114 §4). The mechanical signal available is *"a workflow file exists on this
branch and not on the default branch"* — and that fires **correctly and constantly**
on every feature branch that is mid-review, including this one for `ci.yml` edits. It
would redden on correct in-progress work, which is precisely the criterion DDR-1063
set when it built `ci-docstate-check`; `ci-docstate-check` remains the shape that works
because it asserts an **arithmetic identity**.

The substitute needs no judgment and no tooling: **a workflow whose only triggers are
`schedule` and `workflow_dispatch` does nothing until it is on the default branch, so
the DDR that ships one owes that statement in its own text.** DDR-1097 verified its
workflow three ways and every one of them opened the file.

---

## §6 — NOT CLAIMED

* **NO fix applied**, no file moved, no branch other than `dev/phase1-seyp3n` touched,
  **no PR merged, closed or re-triaged**, and no operator decision taken.
* **NO defect in `open2-hunt.yml`** — its `on:` block, its inputs, its polarity and its
  isolation from the 3-green criterion are all correct, and its triggers were **parsed**
  rather than eyeballed.
* **DDR-1097 is NOT withdrawn or criticised** (§3). Its harness, its `tid` re-check and
  its two-direction local proof all stand; what this records is that its **CI arm was
  structurally unreachable**, which no check inside the file could have seen.
* **NOTHING is claimed about OPEN-2's cause or rate.** No mechanism is named, no
  artefact is produced, **OPEN-2 does not move**, and it remains at **one
  `[schedcheck]` fire ever**. This DDR does not make a hunt happen; it explains why one
  never did.
* **NO code change**, so `kernel.bin` is **not rebuilt** and the size/headroom pair and
  `ci-docstate-check` are unaffected; **179 gates**, **`GLOBAL_FORBIDDEN` 77**, 79 probe
  ELFs, all unchanged.
* **No open issue moves** (OPEN-1/2/12/13 untouched); this is not an apfreeze.
