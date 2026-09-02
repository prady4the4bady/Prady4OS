# DDR-1045 — a Markdown-only commit reddened the aarch64 boot job

**Status:** FIXED
**Artefact:** CI run 33636643304, `arch-bootstrap (aarch64, qemu-system-arm)`, tip `0a1d0ae`.

---

## §1 — The failure, and why attribution is immediate

```
E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/noble/InRelease  403  Forbidden
E: The repository 'https://packages.microsoft.com/repos/azure-cli noble InRelease' is no longer signed.
E: Failed to fetch https://packages.microsoft.com/ubuntu/24.04/prod/dists/noble/InRelease  403  Forbidden
##[error]Process completed with exit code 100
```

`git diff --name-only 5da58b6 0a1d0ae` returns **one file**:
`docs/PRE_LAUNCH_CHECKLIST.md`. The job died in its *first* step, during
`apt-get update`, before installing a package or compiling a line. Nothing about
the commit is reachable from this failure — it is a runner-image repository
returning 403.

This is the same class DDR-1035 already had to guard the shard job against: its
post-gate hash assertion needed `steps.fetch.outcome == 'success'` precisely
because "a shard can die BEFORE the artifact arrives — the toolchain install is
an apt fetch and the runner image carries third-party repos that have returned
403". The class was known; the exposure was not closed.

---

## §2 — Why it takes the whole job down

`apt-get update` exits non-zero if **any** configured repository fails, the step
runs under `set -e`, so a vendor repo nobody uses is a single point of failure
for every job that installs a toolchain. There were **four** such steps in
`ci.yml`: the CMake-parity job, the build job, the shard job, and the multi-arch
bootstrap job. All four were exposed; the aarch64 one happened to draw the short
straw.

---

## §3 — THE FIRST FIX WAS WRONG AND BROKE EVERY JOB

The first version removed `/etc/apt/sources.list.d/*` wholesale, on this stated
assumption — quoted from its own comment:

> "the Ubuntu archives in /etc/apt/sources.list remain"

**That is false on Ubuntu 24.04.** Noble ships the archives in deb822 form at
`/etc/apt/sources.list.d/ubuntu.sources`, and `/etc/apt/sources.list` is a stub.
So the script deleted the archives themselves, `apt-get update` then succeeded
against nothing, and every job died with:

```
E: Unable to locate package llvm
E: Unable to locate package nasm
```

CI run 33650542691: **build, shard-check, arch-bootstrap (aarch64) and
arch-bootstrap (riscv64) all failed**, in both the push and PR suites. The fix
was strictly worse than the failure it replaced — one broken job became four.

**The assumption was never checked against the target image.** That is the whole
lesson: the claim was specific, load-bearing, trivially checkable, and asserted
in a comment as if it were established.

## §4 — The fix, and the argument that it is safe

`tools/ci/apt_prepare.sh`, called by all four steps. It classifies **by content,
not by path**: a file referencing an Ubuntu archive host
(`archive|security|ports.ubuntu.com`, or `ubuntu.com/ubuntu`) is kept; anything
else is removed. That covers both layouts — noble's `ubuntu.sources` and the
legacy `sources.list` — without needing to know which one the image uses.

**Every package these workflows install comes from the Ubuntu archives** —
clang, lld, llvm, nasm, make, qemu-system-*, dosfstools, mtools, e2fsprogs,
ovmf, xorriso, grub-*. Not one comes from a vendor repository, so nothing needed
is lost.

**Two guards, because this script has already been wrong once:**

1. Refuse to proceed if no Ubuntu source survives the filter — rather than
   updating against an empty index, which is exactly how the first version
   failed.
2. After updating, assert `clang` resolves. It is installed by all four callers,
   so if the index is unusable this says why *here*, instead of every caller
   failing later with a confusing "Unable to locate package".

### §4.1 — What this deliberately does NOT do

It does **not** write `apt-get update || true`. That would be one character
cheaper and would also hide **the Ubuntu archives** being unreachable — and a
toolchain installed against a stale index is a genuinely different build. Update
stays fatal; only the unused repositories go away.

## §5 — Verified locally this time

`tools/ci/apt_prepare_selftest.sh`, wired into `hygiene_check.sh` (now **five**
checks) and `make ci-aptprepare-selftest`. The script's directory, main list and
`rm` are overridable, purely so the classifier can be exercised without root and
without a runner image.

| fixture | asserts |
|---|---|
| **noble layout** — `ubuntu.sources` beside `microsoft-prod.list` | `ubuntu.sources` **survives**, microsoft removed, exit 0 |
| **third-party only** | the guard **refuses**, non-zero — no update against an empty index |
| **legacy layout** — archives in `sources.list` | proceeds, exit 0 |

Fixture 1 *is* the regression test for the mistake: it reproduces the exact
layout that broke.

**Mutation-checked.** Reinstating the shipped behaviour (`if false` — remove
everything) fails fixture 1 twice over, and its output is the defect verbatim:

```
[apt_prepare] REMOVE (third-party):    .../ubuntu.sources
[apt_prepare] FAIL: no Ubuntu archive source survived the filter.
FAIL 1: DELETED ubuntu.sources -- this is the bug that broke CI
```

## §6 — What is still not claimed

- **This fixes no code defect**, because there was none. It removes a failure
  mode unrelated to the kernel.
- **CI is not immune to apt.** The Ubuntu archives can still be unreachable, and
  that will still fail the job — deliberately (§4.1).
- **The fixtures are not the runner image.** They encode what noble's layout is
  *believed* to be; the real environment is still only observable in CI. The
  difference from the first attempt is that the belief is now written down as an
  executable test that a mutation demonstrably fails, instead of a sentence in a
  comment.
