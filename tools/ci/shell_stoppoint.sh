#!/usr/bin/env bash
# TASK3-revised measurement: run smoke-shell N times and record WHERE the feeder
# session stops each run.
#
# The gate's "[shell] FAIL: 2>> truncated the earlier entry" is a TRIPWIRE — it
# is the first grep past the point the session dies, so it fires for any early
# stop. What actually discriminates the cause is the STOPPING POINT:
#   varies run-to-run  -> burst-timing input loss (DDR-916 arm2 incomplete)
#   identical every run -> a hard hang at one specific command
#
# Records, per run: serial line count, the last PRISM prompt line (the last
# command the shell actually echoed), and the final serial line.
set -u
cd "$(dirname "$0")/../.."
N="${1:-5}"
mkdir -p build/artifacts
TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/artifacts/stoppoint-$TS.txt"

for i in $(seq 1 "$N"); do
    make smoke-shell >/dev/null 2>&1
    RC=$?
    LINES=$(wc -l < build/shell_serial.log 2>/dev/null || echo 0)
    LASTPROMPT=$(grep -a 'prism>' build/shell_serial.log 2>/dev/null | tail -1)
    LASTLINE=$(tail -1 build/shell_serial.log 2>/dev/null)
    {
        echo "run=$i rc=$RC lines=$LINES"
        echo "  last-prompt: $LASTPROMPT"
        echo "  last-line  : $LASTLINE"
    } | tee -a "$OUT"
    cp -f build/shell_serial.log "build/artifacts/stop-$TS-run$i.log" 2>/dev/null
done
chmod 444 "$OUT" build/artifacts/stop-"$TS"-run*.log 2>/dev/null || true
echo "STOPPOINT_REPORT=$OUT"
