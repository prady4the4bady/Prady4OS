#!/usr/bin/env bash
# DDR-880 / OPEN-10 (item 47) campaign — the run-reader that DDR-880 shipped without.
#
# fs_test_thread is intermittently lost (~6.7% per DDR-880's measured 2/30).
# Boot-stamps A/B/C already carry g_ticks (kernel/main.c:1223/1717/1804):
#     A probe-block-begin   B proofs-begin   C ext4-done
# DDR-880's reading rule:
#     A but no C          -> loss is INSIDE the ext4 block
#     A and C but no B    -> the next elf_load after ext4 is the suspect
#     A, B, C all present -> that run is healthy
#
# Scores each run on the kernel's OWN prints only. Never grep the whole log for
# a string the harness also echoes (that detector bug is what made DDR-880's
# first campaign report false positives).
#
# usage: open10_campaign.sh [runs]   (default 30)
set -u
cd "$(dirname "$0")/../.."
N="${1:-30}"
TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p build/artifacts
OUT="build/artifacts/open10-$TS.txt"
fails=0

for i in $(seq 1 "$N"); do
    start=$(date +%s)
    # boot_test.sh unlinks its SERIAL_LOG on every exit path, so the capture has
    # to be mirrored WHILE the run is live (same pattern as gate_evidence.sh).
    LIVE="build/artifacts/.o10-$TS-$i.live"
    log="build/artifacts/o10-$TS-$i.log"
    rm -f "$LIVE" "$log"
    ( while :; do [ -f "$LIVE" ] && cp -f "$LIVE" "$log" 2>/dev/null; sleep 1; done ) &
    poller=$!
    SERIAL_LOG="$LIVE" make smoke-sfs-btree-smp4 > /tmp/o10.log 2>&1
    rc=$?
    sleep 2; kill "$poller" 2>/dev/null; wait "$poller" 2>/dev/null
    mt=$(stat -c %Y "$log" 2>/dev/null || echo 0)
    if [ "$mt" -ge "$start" ]; then fresh=FRESH; else fresh=STALE; fi
    A=$(grep -ac 'boot-stamp\] A' "$log" 2>/dev/null || echo 0)
    B=$(grep -ac 'boot-stamp\] B' "$log" 2>/dev/null || echo 0)
    C=$(grep -ac 'boot-stamp\] C' "$log" 2>/dev/null || echo 0)
    churn=$(grep -ac '\[sfs\] btree churn OK' "$log" 2>/dev/null || echo 0)
    verdict=healthy
    [ "$rc" -ne 0 ] && { verdict=FAIL; fails=$((fails+1)); }
    line="run=$i rc=$rc $fresh A=$A B=$B C=$C churnOK=$churn verdict=$verdict"
    echo "$line" | tee -a "$OUT"
    if [ "$rc" -ne 0 ]; then
        cp -f "$log" "build/artifacts/open10-$TS-fail$i.log" 2>/dev/null || true
        grep -a 'boot-stamp' "$log" 2>/dev/null | tee -a "$OUT"
    fi
done
echo "TOTAL fails=$fails / $N   report=$OUT" | tee -a "$OUT"
