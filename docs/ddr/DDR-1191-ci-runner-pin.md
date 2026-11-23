# DDR-1191: Pin the CI runner image to `ubuntu-24.04`

Status: **implemented** in this PR. Not a gate change: no arm, sentinel or
bound moves. Platform: GitHub-hosted runners. Nothing here runs on real
hardware.

## 1. Failure (real artefacts)

Every `pradyos-ci` run started on or after 2026-10-31 fails before any boot.
Runs [41166812055](https://github.com/prady4the4bady/Prady4OS/actions/runs/41166812055),
[41166815985](https://github.com/prady4the4bady/Prady4OS/actions/runs/41166815985) and
[41166820283](https://github.com/prady4the4bady/Prady4OS/actions/runs/41166820283)
(three mutant branches on #91 head `60edd34`) all fail the same two jobs:

| Job | Step | Log line |
|---|---|---|
| `build` | Build kernel image (-Werror) | `make: lld-link-18: No such file or directory` / `[Makefile:1197: build/BOOTX64.EFI] Error 127` |
| `arch-bootstrap (riscv64, qemu-system-misc)` | Toolchain | `MISSING: qemu-system-riscv64` |

`build-and-boot` is skipped because `build` failed, so these runs prove nothing
about the kernel. The last green run of the same workflow was
[40893146271](https://github.com/prady4the4bady/Prady4OS/actions/runs/40893146271)
(#81 `ea5e6c1`, 2026-10-29).

## 2. Mechanism

Every job in `ci.yml` and `open2-hunt.yml` ran on `runs-on: ubuntu-latest`.
The runner log header of the failing runs reads `Image: ubuntu-26.04`,
`Version: 20261025.179.1`. The green runs ran on `ubuntu-24.04`. GitHub moved
the `ubuntu-latest` label, and two things the tree depends on changed with it:

- **LLVM 18 to LLVM 21.** `apt_prepare.sh clang lld` now installs
  `clang 1:21.1.6-71`. That package has no `lld-link-18` binary, and
  `Makefile:1198` calls `lld-link-18` by its versioned name.
- **QEMU packaging.** `qemu-system-misc 1:10.2.1` on 26.04 no longer ships
  `qemu-system-riscv64`, which the riscv64 bootstrap job needs.

So the same commit SHA built on Wednesday and fails today. Under the merge
rule (one green `pradyos-ci` run on the exact head SHA), a floating image
label also means a green run is not reproducible: the same SHA can give
another result on a later image.

## 3. Decision

Pin every `runs-on` to `ubuntu-24.04`, the image on which every recorded
result in `docs/ddr/` (runs, mutant reds, timings, static instruction
counts) was measured.

Alternatives considered:

| Option | Why not now |
|---|---|
| Rename `lld-link-18` to `lld-link` and find the new riscv package | It fixes the two named failures only. It moves the whole tree to clang 21 and QEMU 10.2 untested: new `-Werror` diagnostics, and a different TCG under every timing-sensitive arm (OPEN-12/13, smoke-thread placement). That is a migration and needs its own design and its own green runs. |
| Install `clang-18` and `lld-18` explicitly on 26.04 | It keeps the compiler but still changes QEMU, the firmware and the runner kernel under every boot gate. |

**Follow-up (not in this PR):** the move to 26.04 becomes its own checklist
item with a DDR. It gets a versionless `lld-link` (or an `LLD_LINK` variable),
the new riscv QEMU package, a clean clang 21 `-Werror` build, and a full green
run before the pin is lifted. GitHub retires an image label some months after
it publishes the next one, so this is due before `ubuntu-24.04` is removed.

## 4. Evidence and vacuity

- Red before: the three runs in §1, on `ubuntu-26.04`.
- Green after: this PR's own `pradyos-ci` run on its exact head, which must
  show `Image: ubuntu-24.04` in the runner header. The PR records that run.
- This changes no gate, so no mutant applies. The "would it pass on the
  unfixed tree" question is answered by §1: it does not.

## 5. Effect on open PRs

Every open PR whose head predates this change fails CI the same way. Each one
picks the pin up when `dev/phase1` is merged into it after this PR merges, or
it carries the same two-line change before that. Once the base has it, the
change is a no-op.
