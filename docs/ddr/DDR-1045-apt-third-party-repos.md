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

## §3 — The fix, and the argument that it is safe

`tools/ci/apt_prepare.sh`, called by all four steps: remove
`/etc/apt/sources.list.d/*`, then `apt-get update`.

**Every package these workflows install comes from the Ubuntu archives** —
clang, lld, llvm, nasm, make, qemu-system-*, dosfstools, mtools, e2fsprogs,
ovmf, xorriso, grub-*. Not one comes from a vendor repository. The runner image
ships those repos for other people's workflows; here they are pure liability and
they are the only thing that has ever broken this step.

### §3.1 — What this deliberately does NOT do

It does **not** write `apt-get update || true`. That would have been one
character cheaper and it would also hide **the Ubuntu archives** being
unreachable — and a toolchain installed against a stale index is a genuinely
different build, which is exactly the kind of thing this project spends DDRs
chasing afterwards. Update stays fatal. Only the unused repositories go away,
and the script prints each one it removes so a future reader can see what the
environment actually had.

If a workflow ever needs a vendor package it must re-add that repository itself,
and then it owns that repository's availability.

---

## §4 — What is not claimed

- **This is not a fix for a code defect**, because there was none to fix. It
  removes a failure mode that has nothing to do with the kernel.
- **It does not make CI immune to apt.** The Ubuntu archives can still be
  unreachable, and that will still fail the job — deliberately (§3.1).
- **No local verification is possible.** The failure is a property of the GitHub
  runner image, not of this container. What was verified locally: the script
  parses (`bash -n`), the workflow still parses as YAML, all four call sites are
  routed through it, and no bare `apt-get update` remains in `ci.yml`. The real
  test is the next CI run.
