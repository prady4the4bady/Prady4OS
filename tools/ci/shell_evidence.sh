#!/usr/bin/env bash
# smoke-shell evidence run with a preserved, timestamped, read-only artifact.
#
# smoke-shell does NOT use boot_test.sh — it has its own recipe (Makefile:1089)
# that captures serial to build/shell_serial.log, which persists after the run.
# So the artifact is copied AFTER the gate, not polled during it.
#
# The timestamp is expanded HERE, inside the script. Expanding it inline through
# a `wsl -- bash -lc '...'` command line loses it to the outer shell and produces
# an empty filename (build/artifacts/shell-.log).
set -u
cd "$(dirname "$0")/../.."
mkdir -p build/artifacts

# DDR-916 tooling gap: the `[shell] FAIL: …` line that names the failing
# assertion is emitted by the Makefile recipe to STDOUT, not into the guest's
# serial stream. Capturing only the serial log left every failure anonymous —
# several diagnostic cycles were spent guessing which assertion tripped.
# Tee make's output into a companion artifact alongside the serial log.
MAKEOUT="build/artifacts/shell-make-$(date -u +%Y%m%dT%H%M%SZ).log"
make smoke-shell 2>&1 | tee "$MAKEOUT"
RC=${PIPESTATUS[0]}
chmod 444 "$MAKEOUT" 2>/dev/null || true
echo "MAKEOUT=$MAKEOUT"
echo "--- assertion result ---"
grep -a '^\[shell\]' "$MAKEOUT" || echo "(no [shell] line emitted)"

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/artifacts/shell-$TS.log"
if [ -f build/shell_serial.log ]; then
    cp -f build/shell_serial.log "$OUT"
    chmod 444 "$OUT"
    echo "RC=$RC ARTIFACT=$OUT BYTES=$(stat -c%s "$OUT")"
else
    echo "RC=$RC ARTIFACT=MISSING — build/shell_serial.log was never created"
    exit 2
fi
