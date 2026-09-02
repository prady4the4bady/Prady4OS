#!/usr/bin/env bash
# apt_prepare.sh — DDR-1045: make `apt-get update` survive the runner image's
# third-party repositories.
#
# THE FAILURE THIS EXISTS FOR (CI run 33636643304, arch-bootstrap aarch64, tip
# 0a1d0ae -- a commit whose diff is ONE MARKDOWN FILE):
#
#   E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/
#      noble/InRelease  403  Forbidden
#   E: The repository '...' is no longer signed.
#   ##[error]Process completed with exit code 100
#
# `apt-get update` exits non-zero when ANY configured repository fails, the step
# runs under `set -e`, and the job dies before installing a single package or
# compiling a line. Nothing about the commit is involved; the job cannot even
# reach the code.
#
# WHY REMOVING THEM IS SAFE, and this is the whole argument: every package these
# workflows install -- clang, lld, llvm, nasm, make, qemu-system-*, dosfstools,
# mtools, e2fsprogs, ovmf, xorriso, grub-* -- comes from the UBUNTU ARCHIVES.
# Not one comes from a vendor repository. The runner image ships those repos for
# other people's workflows; here they are pure liability, and they are the only
# thing that has ever broken this step.
#
# WHAT THIS DELIBERATELY DOES NOT DO: it does not pass `|| true` to apt-get
# update. A blanket suppression would also hide the Ubuntu archives being
# unreachable, and a toolchain installed from a stale index is a genuinely
# different build. Update stays fatal; only the unused repositories go away.
#
# If a workflow ever needs a vendor package, it must re-add that repository
# itself -- and then it owns the availability of that repository.
set -euo pipefail

removed=0
for f in /etc/apt/sources.list.d/*; do
    [ -e "$f" ] || continue
    echo "[apt_prepare] removing unused third-party repo: $f"
    sudo rm -f "$f"
    removed=$((removed + 1))
done
echo "[apt_prepare] removed $removed third-party repository file(s); the Ubuntu archives in /etc/apt/sources.list remain"

sudo apt-get update
