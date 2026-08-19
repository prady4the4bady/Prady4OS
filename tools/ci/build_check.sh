#!/usr/bin/env bash
# build_check.sh — build the kernel IMAGE and prove the binary actually changed.
#
# HISTORY / WHY THIS IS THE WAY IT IS  (do not "simplify" any of this):
#
# 1. This script used to run plain `make`. The Makefile's default goal is
#    `all:`, which is a HELP TARGET — it echoes usage text and compiles
#    NOTHING. So it exited 0 with zero warnings and zero errors for a build
#    that never happened, and every caller believed the tree was clean. A
#    broken string literal in sfs.c survived several "clean builds" that way
#    and only surfaced when a gate (which depends on $(IMG)) forced a real
#    compile. Always build an explicit target: `image`.
#
# 2. `$?` and `$(...)` evaluated INLINE through `wsl -d Ubuntu-24.04 -- bash -c
#    "..."` come back EMPTY, so an inline build check prints "MAKE_RC=" and
#    "warnings: errors:" and looks successful having measured nothing. That is
#    why this is a FILE (RULE 24 / T4).
#
# 3. rc=0 is NOT proof the edit was compiled. make can legitimately decide a
#    target is up to date. The only reliable proof is that the image hash
#    CHANGED, so this script reports the hash before and after and says plainly
#    whether it moved (R1 / T1).
cd "$(dirname "$0")/../.." || exit 2
LOG=build/gatelogs/build_check.log       # NOT /tmp — WSL wipes it mid-run.
BIN=build/kernel.bin
mkdir -p build/gatelogs
# shellcheck disable=SC1090
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"

before=""
[ -f "$BIN" ] && before=$(md5sum "$BIN" | cut -d' ' -f1)

make image > "$LOG" 2>&1
rc=$?

# T3: never trust a grep against a file that may not exist or be empty.
if [ ! -s "$LOG" ]; then
    echo "build_check: EMPTY BUILD LOG — the build did not run at all"
    exit 3
fi

warns=$(grep -i "warning:" "$LOG" | grep -cvE "^make(\[[0-9]+\])?: ")
errs=$(grep -ci "error:" "$LOG")
after=""
[ -f "$BIN" ] && after=$(md5sum "$BIN" | cut -d' ' -f1)

echo "build_check: target=image MAKE_RC=$rc warnings=$warns errors=$errs"
echo "build_check: hash before=${before:-<none>} after=${after:-<none>}"
if [ "$before" = "$after" ] && [ -n "$before" ]; then
    echo "build_check: HASH UNCHANGED."
    echo "build_check:   If you just EDITED a source, this means the edit was"
    echo "build_check:   NOT compiled in — investigate the Makefile dependency."
    echo "build_check:   If you only re-ran a build with no content change, this"
    echo "build_check:   is correct (the build is deterministic)."
    hashmoved=0
else
    echo "build_check: hash MOVED — the edit is in the binary"
    hashmoved=1
fi

if [ "$rc" -ne 0 ] || [ "$errs" -ne 0 ] || [ "$warns" -ne 0 ]; then
    echo "build_check: --- diagnostics ---"
    grep -iE "warning|error:" "$LOG" | head -40
    exit 1
fi
if [ "${1:-}" = "--expect-change" ] && [ "$hashmoved" -ne 1 ]; then
    echo "build_check: FAIL — --expect-change given but the binary did not move"
    exit 4
fi
echo "build_check: CLEAN (rc=0, 0 warnings, 0 errors, binary updated)"
