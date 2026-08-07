# DDR-859 — CMake/Makefile hybrid build (Group 1 item 3)

**Status:** Accepted — **supersedes DDR-843 Decision 1**
**Date:** 2026-08-07
**Scope:** `CMakeLists.txt`, `Makefile` (`print-flags`, `print-kernel-sources`,
`cmake-check`), `tools/ci/cmake_parity_check.sh` + self-test, Dockerfile, ci.yml.

## Reversal, recorded plainly

DDR-843 Decision 1 skipped CMake for v1.0.0. The operator's brief had reserved
that call — *"get my sign-off before skipping"* — reviewed the reasoning, and
**directed that the hybrid be built**. DDR-843's Decision 1 is superseded by
this DDR, not quietly amended.

My recommendation was wrong to treat "the Makefile is sufficient" as settling
it. Sufficiency was never the question the operator asked.

## The hazard a hybrid actually has

It is not "two build systems". It is **two sets of flags**.

A `CMakeLists.txt` that spelled out `-mcmodel=kernel`, `-mno-red-zone` and
`-Werror` itself would still produce a `kernel.elf`. The 138 gates would keep
passing — they build with the Makefile. And CMake would ship a kernel compiled
differently from the one that was proven. **Neither system would report
anything.** That is this project's recurring defect: silent drift that looks
like success.

## Decision

**The Makefile stays canonical; CMake queries it.**

- `make print-flags` emits the canonical flags machine-readably. One definition.
- `CMakeLists.txt` runs it at configure time and builds real CMake targets from
  the answer. If the query fails, configure **fails** — a fallback to built-in
  defaults is precisely the divergence being prevented.
- `make print-kernel-sources` supplies the source list, and CMake **asserts the
  count** against the Makefile's own. A CMake `file(GLOB)` would be a second
  source list that drifts — the defect that cost DDR-822, -825, -833 and -835.
- Gates and the image are **delegated**, never reimplemented. A CMake target
  re-deriving a gate would be a second definition of what "passing" means.

**`make cmake-check` is the guard.** It configures CMake, then diffs what CMake
captured against what the Makefile reports now.

**Exit 77 when cmake is absent — never 0.** A check returning success when it
could not run is indistinguishable from one that ran and found nothing. Same
discipline as `tools/vbox_runner/run_vbox.sh`.

**`-Werror`, `-mcmodel=kernel` and `-mno-red-zone` are asserted BY NAME**, not
left to the diff. A diff only fires when CMake's copy is stale; if *both* sides
lost `-Werror` together it would report parity while the zero-warnings mandate
had quietly ended.

## Verification

cmake is not installed on this build host, so the configure path is CI-only
here. Shipping the drift logic unverified on that basis would repeat DDR-854 —
a check nobody had watched reject anything — so
`tools/ci/cmake_parity_selftest.sh` exercises every path that does not need
cmake:

| arm | expected | got |
|---|---|---|
| identical flags | PASS | ✅ |
| one flag differs | DRIFT | ✅ |
| **`-Werror` dropped from BOTH sides** | **CONTRACT** | ✅ |
| required variable missing | DRIFT | ✅ |
| stale kernel source count | DRIFT | ✅ |

The third arm is the one a plain diff cannot catch.

`cmake` added to the Dockerfile; `make cmake-check` and the self-test wired into
the host-only CI job, which runs on `ubuntu-latest` and therefore **does**
exercise the configure path. `ci-shard-check` (134 gates) and `ci-docker-check`
still pass.

## Scope, stated so it is not over-read

CMake compiles the kernel objects and drives the delegated targets. It does
**not** own the 138 gate recipes, the image sequencing, or the release. Porting
those would rewrite the machinery that proves every release, and nothing in the
hybrid requires it.
