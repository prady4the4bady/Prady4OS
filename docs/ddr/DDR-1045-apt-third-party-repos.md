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

## §3 — TWO FAILED FIXES, BOTH FROM GUESSING AT THE RUNNER IMAGE

The pattern matters more than either bug, so both are recorded.

**Attempt 1 — removed `/etc/apt/sources.list.d/*` wholesale**, on this
assumption, quoted from its own comment:

> "the Ubuntu archives in /etc/apt/sources.list remain"

False on Ubuntu 24.04: noble ships them as deb822 at
`/etc/apt/sources.list.d/ubuntu.sources`, and `sources.list` is a stub. So it
deleted the archives, `apt-get update` succeeded against nothing, and CI run
33650542691 failed **build, shard-check, aarch64 and riscv64** with
`E: Unable to locate package llvm`. One broken job became four.

**Attempt 2 — kept files whose CONTENT matched an Ubuntu archive host.** Better
shaped, and still a guess about a file this environment cannot read. The
runner's real `ubuntu.sources` did not match the pattern:

```
[apt_prepare] REMOVE (third-party):    /etc/apt/sources.list.d/ubuntu.sources
[apt_prepare] FAIL: no Ubuntu archive source survived the filter.
```

The guard worked exactly as designed — it refused rather than updating an empty
index, so this time nothing was silently broken. But the arch jobs still failed
(CI 33650946252). **The guard converted a wrong guess into a loud refusal
instead of a silent catastrophe, which is the only reason attempt 2 was better
than attempt 1.**

Both attempts share one root cause: **a claim about the runner image, asserted
without being able to check it.** Twice.

## §4 — The fix that assumes nothing

`apt_prepare.sh <package>...`. It does not read, classify, or delete any
repository file. It:

1. runs `apt-get update` and **tolerates failure**, because a vendor-repo 403 is
   not this project's problem and is the only thing that has ever broken here;
2. then **proves the index usable** by resolving every package the caller
   actually needs;
3. then installs them.

The post-check is what makes step 1 safe, and it is the proper answer to the
objection the earlier versions were built around — *"`|| true` would also hide
the Ubuntu archives being unreachable."* It would, **unless something afterwards
checks**. Something does now, and it checks the thing that actually matters —
can we install what this job needs? — rather than inferring it from a filename
or a URI pattern.

A missing candidate fails loudly and says it is *not* a vendor 403, so the next
reader is not sent down the wrong path.

## §5 — Verified locally, with stubs, and mutation-checked

`tools/ci/apt_prepare_selftest.sh` — wired into `hygiene_check.sh` (now **five**
checks) and `make ci-aptprepare-selftest`. `apt-get` and `apt-cache` are stubbed
on `PATH`, so no root and no real index are involved.

| fixture | asserts |
|---|---|
| update clean, packages resolve | installs, rc=0 |
| **update FAILS**, packages resolve | **still installs, rc=0** — the vendor-403 case, the reason this exists |
| update clean, a package has **no candidate** | **refuses**, rc≠0, names the cause, installs nothing |
| **apt-cache prints NOTHING** | **refuses** — real apt's answer for an *unknown* package |
| no arguments | usage error, never a silent no-op |

**Mutants, each landing on its own fixture:**

| | mutation | fails |
|---|---|---|
| M1 | drop the resolve check | fixture 3 — *"proceeded with an UNUSABLE index — attempt 1's bug"* |
| M2 | make the update failure fatal again | fixture 2 — *"a vendor-repo update failure must NOT fail the job"* |
| M3 | drop **only** the empty-output branch | fixture 5 — fixture 3 still passes, so the two shapes are independently covered |

M1 is attempt 1's defect reproduced; M2 is the original failure reproduced.

### §5.1 — Checking real apt closed a gap in this test

`apt-cache policy` was run against **real apt** rather than assumed, and it has
**two** failure shapes, not one:

```
$ apt-cache policy clang
  Candidate: 1:18.0-59~exp2
$ apt-cache policy definitely-not-a-real-package-xyz
                                    <- NOTHING. Not "Candidate: (none)".
```

`Candidate: (none)` is what a *known* package with nothing installable prints; an
**unknown** package prints empty output. `apt_prepare.sh` handles both — the
`elif` branch exists for exactly this — but the stub originally only ever emitted
the first shape, so **the branch covering real apt's actual behaviour for a
missing package was never exercised**. Fixture 5 and M3 close it.

Given this DDR is about two fixes shipped on unchecked assumptions about the
environment, checking this one before trusting it seemed the minimum. It was a
real gap: without fixture 5 a script that ignored empty output would have passed
the whole suite and then failed in CI on any absent package or name typo,
printing `index OK` immediately before apt said `Unable to locate package`.

## §6 — What is still not claimed

- **This fixes no code defect**, because there was none.
- **CI is not immune to apt.** If the Ubuntu archives are genuinely unreachable,
  the resolve check fails the job — deliberately.
- **The stubs are not apt.** They exercise this script's decision logic, not
  apt's behaviour. What is different from attempts 1 and 2 is that the script no
  longer depends on any belief about the runner image, so there is no longer a
  guess for the environment to falsify.
