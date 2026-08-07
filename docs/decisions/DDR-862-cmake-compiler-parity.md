# DDR-862 — the parity check compared flags but not the compiler

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Fixes DDR-859. `CMakeLists.txt`, `Makefile` (`print-flags`),
`tools/ci/cmake_parity_check.sh` + self-test, `ci.yml`.

## What CI caught

Runs `31139499754`, `31139497587` (on `c99f1f8`) and `31140901381` (on
`39fa1cf`) failed `shard-check` at the new parity step. Two distinct bugs, and
the second is the one that matters.

### Bug 1 — a declared language that is never used

`project(pradyos C ASM_NASM)` made configure hard-fail on a runner without
nasm:

    No CMAKE_ASM_NASM_COMPILER could be found.

CMake here compiles **C objects only**; assembly is delegated to the Makefile.
Declaring `ASM_NASM` was aspirational, and it created a dependency on a tool
this file never invokes. Removed.

### Bug 2 — CMake was building with gcc, and the guard said parity held

The configure log:

    -- The C compiler identification is GNU 13.3.0

`set(CMAKE_C_COMPILER clang)` came **after** `project()`. CMake resolves the
compiler *during* `project()`, so the assignment was ignored and the system
default won. The Makefile builds with clang (`tools/build/toolchain.mk`).

**And `make cmake-check` reported parity**, because every flag string matched.
Nothing compared the compiler.

That is the DDR-859 defect one level below where the guard was looking. I wrote
that DDR arguing the hybrid's real hazard is "two sets of flags" — correct as
far as it went, and I then built a check that only tested that sentence.
Identical flags through a different compiler is not parity; it is two different
kernels. `-mcmodel=kernel` and `--target=x86_64-elf` do not even mean the same
thing to gcc.

Worth being precise about the failure mode: this could not have shipped a bad
kernel today, because CMake does not produce the release artefact — the Makefile
does. But the entire purpose of the hybrid is that CMake *can* build the kernel,
and it was quietly configured to build a different one.

## Fixes

1. **`ASM_NASM` removed** from `project()`.
2. **Order is load-bearing**: `make print-flags` is queried and
   `CMAKE_C_COMPILER` is set **before** `project()`. If the query fails, or
   reports no `CC`, configure aborts — no guessed toolchain.
3. **`CC`, `LD` and `NASM` are part of the flag contract**, emitted by
   `print-flags` and required by both CMake and the checker.
4. **CMake asserts its resolved compiler matches** the Makefile's, by basename,
   and `cmake_parity_check.sh` re-asserts it from `CMakeCache.txt`.
5. **`clang` is installed explicitly** in the `shard-check` job. That job
   installs no toolchain otherwise, and "the runner image happened to carry it"
   is not a build dependency.

## Verification

`cmake_parity_selftest.sh` grew from 5 arms to **7**, and both new arms model
this bug directly:

| arm | expected | got |
|---|---|---|
| identical flags | PASS | ✅ |
| one flag differs | DRIFT | ✅ |
| `-Werror` dropped from both sides | CONTRACT | ✅ |
| required variable missing | DRIFT | ✅ |
| stale kernel source count | DRIFT | ✅ |
| **compiler differs (clang vs gcc)** | **DRIFT** | ✅ |
| **`CC` absent from the contract** | **DRIFT** | ✅ |

## The lesson, stated plainly

DDR-859's self-test verified the checker against the failure I had *thought of*.
It passed 5/5 and was shipped, and the failure I had not thought of was found by
CI four minutes later. A self-test is bounded by its author's imagination in a
way a real run is not — which is an argument for landing guards early and
watching them fail, not for trusting them once they pass.
