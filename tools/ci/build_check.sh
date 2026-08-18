#!/usr/bin/env bash
# build_check.sh — build the kernel and report rc + warning/error counts.
# RULE 24, same reason as check_hash.sh and ab_rate.sh: `$?` and `$(...)`
# evaluated INLINE through `wsl -d Ubuntu-24.04 -- bash -c "..."` come back as
# EMPTY STRINGS. A build check written inline therefore prints "MAKE_RC=" and
# "warnings: errors:" and looks like it succeeded while having measured nothing.
# Arithmetic and command substitution must live in a file to be trustworthy.
cd "$(dirname "$0")/../.." || exit 2
LOG=build/gatelogs/build_check.log      # NOT /tmp — WSL wipes it mid-run.
mkdir -p build/gatelogs
# shellcheck disable=SC1090
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"

make -j4 > "$LOG" 2>&1
rc=$?

warns=$(grep -ci "warning" "$LOG")
errs=$(grep -ci "error:" "$LOG")

echo "build_check: MAKE_RC=$rc warnings=$warns errors=$errs"
echo "build_check: log=$LOG"
if [ "$rc" -ne 0 ] || [ "$errs" -ne 0 ] || [ "$warns" -ne 0 ]; then
    echo "build_check: --- first 40 diagnostic lines ---"
    grep -iE "warning|error:" "$LOG" | head -40
    exit 1
fi
echo "build_check: CLEAN (zero warnings, zero errors, rc=0)"
