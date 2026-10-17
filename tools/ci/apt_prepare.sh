#!/usr/bin/env bash
# apt_prepare.sh — DDR-1045: install packages without the runner image's
# third-party repositories being able to fail the whole job.
#
# usage: apt_prepare.sh <package>...
#
# THE FAILURE (CI 33636643304, arch-bootstrap aarch64, tip 0a1d0ae -- a commit
# whose diff is one Markdown file):
#
#   E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/
#      noble/InRelease  403  Forbidden
#   ##[error]Process completed with exit code 100
#
# `apt-get update` exits non-zero when ANY configured repository fails and the
# steps run under `set -e`, so a vendor repo no workflow here uses was a single
# point of failure for every toolchain install -- four steps in ci.yml.
#
# TWO FAILED ATTEMPTS PRECEDED THIS ONE, BOTH FROM GUESSING AT THE RUNNER IMAGE
# INSTEAD OF CHECKING IT. They are recorded because the pattern matters more
# than either bug:
#
#   1. Remove /etc/apt/sources.list.d/* wholesale, assuming "the Ubuntu archives
#      in /etc/apt/sources.list remain". FALSE on Ubuntu 24.04 -- noble ships
#      them as deb822 at sources.list.d/ubuntu.sources. Deleted the archives;
#      every package became unavailable; build + shard-check + aarch64 + riscv64
#      all failed (CI 33650542691).
#   2. Keep files whose CONTENT matches an Ubuntu archive host. The runner's real
#      ubuntu.sources did not match the pattern -- so the guard correctly refused
#      and the arch jobs still failed (CI 33650946252). Still a guess about a
#      file this environment cannot read.
#
# SO THIS VERSION ASSUMES NOTHING ABOUT THE SOURCES AT ALL. It does not read,
# classify, or delete any repository file. Instead:
#
#   - `apt-get update` is allowed to fail, because a vendor repo 403 is not this
#     project's problem and is the only thing that has ever broken here;
#   - and then the index is PROVED USABLE by resolving every package the caller
#     actually needs, before installing them.
#
# That post-check is what makes tolerating the update failure safe, and it is the
# answer to the objection the earlier versions were built around: "|| true would
# also hide the Ubuntu archives being unreachable". It would -- unless something
# afterwards checks. Something does, and it checks the exact thing that matters
# (can we install what this job needs?) rather than inferring it from a filename
# or a URI pattern.
set -uo pipefail

if [ "$#" -eq 0 ]; then
    echo "[apt_prepare] usage: apt_prepare.sh <package>..." >&2
    exit 2
fi

SUDO="${APT_SUDO-sudo}"

# Tolerated: individual repositories may 403. Not tolerated silently -- the
# outcome is reported, and the resolve check below is what decides.
if $SUDO apt-get update; then
    echo "[apt_prepare] apt-get update: clean"
else
    echo "[apt_prepare] apt-get update reported errors (a vendor repo is usually" \
         "the cause); continuing to the resolve check, which is what actually decides"
fi

# PROVE the index is usable for exactly what this job installs. A package with
# no candidate means the archives themselves are missing or stale, and failing
# HERE names the cause instead of leaving the caller with a bare
# "E: Unable to locate package".
# DDR-1048: the output is captured ONCE into a variable and matched as a STRING.
# It used to be two `apt-cache policy | grep -q` pipelines, and that was a RACE
# that could fail CI on a package which resolves perfectly well:
#
#   grep -q exits at the FIRST match and closes the pipe, so apt-cache is killed
#   by SIGPIPE and exits 141. This script runs under `set -o pipefail`, so the
#   pipeline is NON-ZERO even though grep matched. The `!` branch then reads that
#   as "no Candidate:" and marks the package missing. Measured on a real Ubuntu
#   24.04 host: PIPESTATUS=(141 0) for clang on one run, for nasm and xorriso on
#   another -- WHICH package loses the race varies, so it is an intermittent red.
#
# It was invisible to the stub selftest because the stubs emit a few bytes from a
# shell function, so grep -q drains them before exiting and SIGPIPE never fires.
# Only running against real apt showed it (DDR-1048).
missing=""
for p in "$@"; do
    pol="$(apt-cache policy "$p" 2>/dev/null)"
    case "$pol" in
        "")                  missing="$missing $p" ;;   # unknown: apt printed nothing
        *"Candidate: (none)"*) missing="$missing $p" ;; # known, uninstallable
        *"Candidate:"*)      : ;;                       # resolves
        *)                   missing="$missing $p" ;;
    esac
done
if [ -n "$missing" ]; then
    echo "[apt_prepare] FAIL: no candidate for:$missing" >&2
    echo "[apt_prepare] The package index is unusable -- this is NOT a vendor-repo" >&2
    echo "[apt_prepare] 403, it means the Ubuntu archives are missing or stale." >&2
    echo "[apt_prepare] Configured sources:" >&2
    ls -la /etc/apt/sources.list.d/ >&2 || true
    exit 1
fi
echo "[apt_prepare] index OK — every requested package resolves:$(printf ' %s' "$@")"

$SUDO apt-get install -y --no-install-recommends "$@"
