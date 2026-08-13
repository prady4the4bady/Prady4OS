#!/usr/bin/env bash
# Generic evidence run: boot a gate with a preserved, read-only serial artifact.
#
# boot_test.sh unlinks its serial capture on every exit path, so the artifact
# must be copied WHILE the run is live. A background poller mirrors the growing
# capture; the last mirror survives cleanup and is frozen read-only.
#
# usage: _gate_evidence.sh <tag> <timeout_s> [VAR=VAL ...]
set -u
cd "$(dirname "$0")/../.."
TAG="$1"; shift
TMO="$1"; shift
TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p build/artifacts
LIVE="build/artifacts/.$TAG-$TS.live"
OUT="build/artifacts/$TAG-$TS.log"

( while :; do [ -f "$LIVE" ] && cp -f "$LIVE" "$OUT" 2>/dev/null; sleep 1; done ) &
POLLER=$!

env "$@" SERIAL_LOG="$LIVE" TIMEOUT_S="$TMO" \
  bash tools/qemu_runner/boot_test.sh build/pradyos.img
RC=$?

sleep 2; kill "$POLLER" 2>/dev/null; wait "$POLLER" 2>/dev/null
chmod 444 "$OUT" 2>/dev/null || true
echo "RC=$RC ARTIFACT=$OUT"
exit $RC
