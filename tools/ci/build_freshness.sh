#!/usr/bin/env bash
# build_freshness.sh — refuse to test a build that make never actually rebuilt.
#
# WHY THIS EXISTS
#
# `rsync --checksum --no-times` into a build tree writes new CONTENT while
# leaving the destination mtime older than the existing build/kernel.bin. make
# then compares timestamps, sees every source older than the artifact, and
# reports "nothing to be done" — so `make image` succeeds WITHOUT COMPILING and
# every gate afterwards tests the previous binary.
#
# Measured, not theorised:
#     user/surfacetest.c   src=1786479678
#     build/kernel.bin     obj=1786482125     (2447s NEWER than its own source)
#     make -q image        -> UP TO DATE
#     touch + make -n      -> recompile IS scheduled   (dep graph is fine)
#
# The dependency graph is correct — $(KERNEL_BIN) lists every source and header.
# Only the timestamps lie. That cost a multi-hour session in which four separate
# source-level hypotheses were each "refuted by measurement", because the binary
# under test never reflected the source being edited.
#
# USAGE
#     bash tools/ci/build_freshness.sh          # after syncing, before building
#
# Exits non-zero if the tree would build nothing, which is the one state that
# must never be trusted. Pass --fix to force the rebuild instead of failing.
set -u
cd "$(dirname "$0")/../.." || exit 1

fix=0
[ "${1:-}" = "--fix" ] && fix=1

if [ ! -e build/kernel.bin ]; then
    echo "build-freshness: OK — no prior artifact, the build cannot be stale"
    exit 0
fi

if make -q image 2>/dev/null; then
    # make believes there is nothing to do. After a sync that changed content,
    # that is a false negative, and it is indistinguishable from a genuinely
    # unchanged tree — so the only safe response is to force the rebuild.
    if [ "$fix" -eq 1 ]; then
        echo "build-freshness: make reports UP TO DATE — forcing a rebuild"
        find kernel user boot -type f \
             \( -name '*.c' -o -name '*.h' -o -name '*.asm' \) -exec touch {} +
        echo "build-freshness: sources touched; make will now rebuild"
        exit 0
    fi
    echo "build-freshness: FAIL — make reports UP TO DATE with an existing"
    echo "                 build/kernel.bin. If anything was synced or edited,"
    echo "                 that binary does NOT reflect the current source."
    echo "                 Re-run with --fix, or build in a fresh directory."
    exit 1
fi

echo "build-freshness: OK — make has work to do, the build will reflect the source"
exit 0
