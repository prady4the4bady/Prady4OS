#!/usr/bin/env bash
# apt_prepare.sh — DDR-1045: make `apt-get update` survive the runner image's
# third-party repositories, WITHOUT deleting the Ubuntu archives.
#
# ORIGINAL FAILURE (CI 33636643304, arch-bootstrap aarch64, tip 0a1d0ae -- a
# commit whose diff is one Markdown file):
#
#   E: Failed to fetch https://packages.microsoft.com/repos/azure-cli/dists/
#      noble/InRelease  403  Forbidden
#   ##[error]Process completed with exit code 100
#
# `apt-get update` exits non-zero when ANY configured repository fails and the
# step runs under `set -e`, so a vendor repo no workflow here uses was a single
# point of failure for every toolchain install.
#
# THE FIRST FIX WAS WRONG, AND THIS COMMENT EXISTS SO IT IS NOT REPEATED. It
# removed /etc/apt/sources.list.d/* wholesale, on the stated assumption that
# "the Ubuntu archives in /etc/apt/sources.list remain". THAT IS FALSE ON UBUNTU
# 24.04: noble ships the archives in deb822 form at
# /etc/apt/sources.list.d/ubuntu.sources, and /etc/apt/sources.list is a stub.
# So the script deleted the archives, `apt-get update` then succeeded against
# nothing, and every job failed with
#
#   E: Unable to locate package llvm
#   E: Unable to locate package nasm
#
# on CI 33650542691 -- worse than the failure it replaced, because it broke all
# four jobs instead of one. The assumption was never checked against the image.
#
# So: classify by CONTENT, not by path. A file that references an Ubuntu archive
# host is kept; anything else goes. Every package these workflows install --
# clang, lld, llvm, nasm, make, qemu-system-*, dosfstools, mtools, e2fsprogs,
# ovmf, xorriso, grub-* -- comes from those archives, so nothing needed is lost.
#
# TWO GUARDS, because this script has already been wrong once:
#   - refuse to proceed if no Ubuntu source survives the filter;
#   - after updating, assert a package every caller installs actually resolves.
# Both fail loudly. `apt-get update` itself stays fatal: a blanket `|| true`
# would also hide the Ubuntu archives being unreachable, and a toolchain built
# against a stale index is a different build.
set -euo pipefail

# Overridable so the classification can be tested without root and without a
# real runner image -- see tools/ci/apt_prepare_selftest.sh. This script has
# already been wrong once in a way no local check would have caught, and the
# fixtures are the only verification available before CI sees it.
SRCDIR="${APT_SOURCES_DIR:-/etc/apt/sources.list.d}"
MAINLIST="${APT_MAIN_LIST:-/etc/apt/sources.list}"
RM="${APT_RM:-sudo rm -f}"

kept=0
removed=0
shopt -s nullglob
for f in "$SRCDIR"/*; do
    [ -f "$f" ] || continue
    if grep -qE '(archive|security|ports)\.ubuntu\.com|ubuntu\.com/ubuntu' "$f"; then
        echo "[apt_prepare] KEEP   (Ubuntu archive): $f"
        kept=$((kept + 1))
    else
        echo "[apt_prepare] REMOVE (third-party):    $f"
        $RM "$f"
        removed=$((removed + 1))
    fi
done

# /etc/apt/sources.list still carries the archives on older images; on noble it
# is a stub and ubuntu.sources above is the real one. Either satisfies this.
if [ "$kept" -eq 0 ] && ! grep -qE '^\s*deb\s' "$MAINLIST" 2>/dev/null; then
    echo "[apt_prepare] FAIL: no Ubuntu archive source survived the filter." >&2
    echo "[apt_prepare] Refusing to update against an empty index -- that is how" >&2
    echo "[apt_prepare] the first version of this script broke every job." >&2
    exit 1
fi
echo "[apt_prepare] kept $kept Ubuntu source file(s), removed $removed third-party file(s)"

[ -n "${APT_PREPARE_SELFTEST:-}" ] && { echo "[apt_prepare] selftest: stopping before apt"; exit 0; }

sudo apt-get update

# Post-check: clang is installed by all four callers. If it does not resolve,
# the index is wrong and every caller is about to fail with a confusing
# "Unable to locate package" -- say why here instead.
if apt-cache policy clang 2>/dev/null | grep -q 'Candidate: (none)'; then
    echo "[apt_prepare] FAIL: 'clang' has no candidate after update -- the package" >&2
    echo "[apt_prepare] index is not usable. Sources still configured:" >&2
    ls -la "$SRCDIR" >&2 || true
    exit 1
fi
echo "[apt_prepare] index OK (clang resolves)"
