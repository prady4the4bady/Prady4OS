# DDR-1187: CI cost — cancel superseded PR runs; docs-only PRs skip the boot shards

Status: proposed; the operator reviews (#45 comment 6436706603, item 5). Not
merged until the operator approves it and the mutants below are shown red in CI.

## 1. The cost, measured from the run list

- A full `pradyos-ci` run is ten `build-and-boot` shards of about 35 minutes
  each, after one `build` job. Wall clock is about 2 hours, because runs queue
  first in, first out behind each other.
- Every push to a PR starts a new run, and the run on the old head keeps
  running. Only the newest head can be merged (one green run on the exact head
  SHA, merge rule 6229808666 item 1), so the old run is pure cost. On
  2026-10-27 three such runs were cancelled by hand (40456699062, 40456875368,
  40536888276).
- A PR that changes only Markdown (a DDR, the handoff, the checklist) still
  builds the kernel and boots it ten times. No build or boot gate reads those
  files (section 3).

## 2. Constraints from the order

- No `paths-ignore` and no `[skip ci]`: a required check that never starts stays
  Pending and blocks the merge.
- Every required check keeps reporting under its own name. The `protect-core`
  ruleset requires `build`, `shard-check` and `build-and-boot (shard 0)` to
  `(shard 9)`.
- The ruleset is not changed. C11.1 stays last.

## 3. Design

### 3.1 Concurrency (item 5a)

```yaml
concurrency:
  group: pradyos-ci-${{ github.event_name == 'pull_request' && format('pr-{0}', github.event.pull_request.number) || github.run_id }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}
```

- One group per PR number, so a new head cancels the run on the old one.
- A push run (`dev/phase1`, `main`) or a `workflow_dispatch` run (a mutant, or a
  second run on a promotion SHA) gets a group of its own, its run id, so nothing
  cancels it. A dispatch on the same SHA as a push run therefore still gives the
  independent second run the promotion rule needs.

### 3.2 Docs-only classification (item 5b)

`tools/ci/docs_only.sh` reads the changed paths on stdin and prints `true` only
when every path is a `*.md` file under `docs/` or at the root. Anything else,
including an empty list, is `false`.

- `*.md` only: `docs/legal/EULA.txt` and `PRIVACY.txt` ride on the ISO and the
  ISO gate compares them (`Makefile`, the `cmp ... docs/legal` line).
- Root Markdown counts: `CLAUDE.md` and `SESSION_HANDOFF.md` are checked by
  `shard-check` (doc state, doc size), and `shard-check` always runs.
- I searched the Makefile, `tools/qemu_runner/` and `tools/ci/run_shard.sh` for
  reads of `.md` files. The only reader under `tools/ci/` is
  `status_report.sh`, and nothing calls it.

The `build` job classifies first, in a step, and exports `docs_only`:

- only `pull_request` events are classified; push and dispatch always run in full;
- the diff is the PR's test merge commit against its first parent
  (`git diff --name-only --no-renames HEAD^1 HEAD`), so a rename out of
  `kernel/` still lists the `kernel/` path; with no second parent the run is full;
- the classifier is read from the **base** side (`HEAD^1:tools/ci/docs_only.sh`),
  so a PR cannot weaken the classifier that judges it; when the base has no
  classifier (this PR), the run is full;
- a git error fails the step, so `build` is red and the merge is blocked.

On `true`, every later step of `build` and of each shard is skipped by a
**step-level** `if:`. The jobs still run and report success under their exact
names; each shard logs that it ran no gate.

**Why steps, not the job-level `if:` the order names.** I disagree with the job
level for the matrix job, because a skipped matrix job may report one check
under its unexpanded name instead of `build-and-boot (shard 0)` to `(shard 9)`,
which would leave the ten required checks Pending — the failure the order
forbids. [Likely; not tested here.] Step-level conditions avoid depending on that
behaviour at all. The cost is ten short jobs (a runner start and one `echo`) per
docs-only PR, instead of ten 35-minute boots. I also kept the classifier as a
step of `build`, not a separate job: a separate job that failed would skip its
dependants, and a skipped required check counts as passing, so the failure mode
would be "no gates ran, merge allowed". As a step, a failure makes `build` red.

## 4. Gate and mutants

`tools/ci/docs_only_selftest.sh` runs in `shard-check` (required). Arm A expects
`true`; every other arm expects `false`, because those are the inputs on which a
wrong `true` would skip gates a change needs.

| Arm | Input | Want |
|---|---|---|
| A | `docs/ddr/DDR-1.md`, `CLAUDE.md`, `docs/a/b/c.md` | true |
| B | `kernel/proc/sched.c` | false |
| C | empty | false |
| D | two docs, then `user/shmtest.c` | false |
| E | `docs/legal/EULA.txt` | false |
| F | `Makefile` | false |
| G | `.github/workflows/ci.yml` | false |
| H | `kernel/README.md` | false |

Vacuity: on the tree before this change the self-test cannot pass, because the
classifier does not exist. Each mutant was checked locally to turn exactly its
designed arm red. They must be shown red in CI (dispatched, read at `shard-check`)
before merge:

| Mutant | Change | Red arm (local) |
|---|---|---|
| M1 | an empty list answers `true` | C |
| M2 | the first docs path answers for the whole list | D |
| M3 | any file under `docs/` counts (`docs/*`) | E |
| M4 | a root-level file of any type counts | F |
| M5 | Markdown anywhere counts (`*/*` arm removed) | H |
| M6 | always `true` | B-H |

The workflow wiring itself (the concurrency group, the step conditions) cannot
be mutated in a self-test. It is checked by observation and labelled until then:

- this PR's own run must log `docs_only=false` and run all ten shards (the base
  has no classifier);
- after merge, the first docs-only PR must log `docs_only=true` with every shard
  printing that it ran no gate, and the first code PR must log `false`;
- a second push to one PR must cancel the run on its first head.

Until each is seen in a run, it is **not verified**.

## 5. Risks

- A gate that starts reading a `.md` file would be skipped on docs-only PRs.
  Arm A's list is the contract; such a gate must also make its file non-docs in
  `docs_only.sh` (the self-test would need a new arm).
- A docs-only PR's green shard checks mean "nothing to run", not "gates passed".
  The shard log says so, and the merge rule's evidence for code is unchanged.
- `ci.yml` itself comes from the PR, as it always has; any change to it is a
  code change (arm G) and runs in full.

## 6. Honesty labels

- The classifier is **gated** by `shard-check` (self-test arms A-H) once M1-M6
  are shown red in CI.
- The workflow wiring is **not verified** until the observations in section 4.
- This touches CI only; there is nothing emulated or hardware-related in it.
