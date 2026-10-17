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
mkdir -p build/artifacts

# DDR-1060 §10: RESUMABLE, because in this environment a campaign CANNOT run
# unattended to completion.
#
# Measured, three times: a 30-run campaign launched in the background is killed
# after 1-5 runs when the session goes idle -- the container executes only while
# a turn is live, and `setsid` does not change that (attempt 3 died after ONE
# run exactly as attempts 1 and 2 did). Restarting from zero each time threw
# away every completed run; three attempts produced 7 runs and a usable N of 0.
#
# So the unit of work is a CHUNK, and the report is the accumulator. Pass an
# existing report as $2 to continue it: the pin is re-checked against that
# report's own kernel_pinned line, so a resume onto a different binary is
# refused for the same reason a mid-campaign rebuild is (a count over two
# binaries is not a measurement). N is the TARGET TOTAL, not the chunk size --
# the loop stops as soon as the report holds N runs, so repeated invocations
# converge instead of overshooting.
# TS names this CHUNK's per-run capture files. It must be set on BOTH paths:
# the script runs under `set -u`, and the resume path originally left it unset,
# so the first use inside the loop killed the chunk after printing "resuming:"
# and recording nothing. Per-chunk rather than per-campaign is correct anyway --
# a resumed chunk must not overwrite an earlier chunk's captures.
TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${2:-}"
if [ -n "$OUT" ] && [ -f "$OUT" ]; then
    RESUME=1
else
    OUT="build/artifacts/open10-$TS.txt"
    RESUME=0
fi
fails=0

# DDR-1060 §9: PIN THE BINARY, AND CHECK IT EVERY RUN.
#
# This tool invokes `make`, so it REBUILDS if any source changed while it was
# running -- and it did: a campaign begun on kernel 46016bc8c7c7fa3b silently
# switched binaries mid-run when kernel/lock_stat.c was edited in another
# window, and nothing in the report said so. Runs before and after the rebuild
# were pooled into one number that describes no binary at all.
#
# DDR-1023 already recorded the rule ("hash-verified before AND after every
# run") after a campaign whose captures turned out to be make output rather
# than serial logs. The rule was written down and never implemented HERE, which
# is the whole failure mode: a discipline that lives only in prose.
#
# ABORT rather than warn. A campaign's entire value is that every run bounds the
# SAME artefact; once that is broken there is no partial result worth keeping,
# and a warning in a 30-run log is a warning nobody reads.
if [ ! -f build/kernel.bin ]; then
    echo "open10_campaign: build/kernel.bin missing -- build first" >&2
    exit 2
fi
PIN=$(sha256sum build/kernel.bin | cut -d" " -f1)
if [ "$RESUME" = "1" ]; then
    WAS=$(sed -n 's/^kernel_pinned=//p' "$OUT" | head -1)
    if [ "$WAS" != "$PIN" ]; then
        echo "open10_campaign: REFUSING to resume onto a different binary" >&2
        echo "  report pinned=$WAS" >&2
        echo "  on disk      =$PIN" >&2
        echo "  Pooling runs across two binaries measures nothing. Start a new" >&2
        echo "  campaign, or rebuild the pinned binary first (DDR-1060 §10)." >&2
        exit 4
    fi
    done_n=$(grep -c "^run=" "$OUT" 2>/dev/null || true)
    [ -z "$done_n" ] && done_n=0
    echo "resuming: $done_n of $N already recorded, kernel=$PIN" | tee -a "$OUT"
else
    done_n=0
    echo "kernel_pinned=$PIN" | tee -a "$OUT"
fi

hash_check() {   # $1 = when (before|after), $2 = run index
    local now
    now=$(sha256sum build/kernel.bin 2>/dev/null | cut -d" " -f1)
    if [ "$now" != "$PIN" ]; then
        echo "open10_campaign: ABORT -- kernel.bin changed $1 run $2" | tee -a "$OUT"
        echo "  pinned=$PIN" | tee -a "$OUT"
        echo "  now   =$now" | tee -a "$OUT"
        echo "  Every run so far bounds a binary that is no longer on disk." | tee -a "$OUT"
        echo "  The campaign is VOID -- do not report its count. Rebuild, then" | tee -a "$OUT"
        echo "  restart on one pinned binary (DDR-1060 §9)." | tee -a "$OUT"
        exit 3
    fi
}

if [ "$done_n" -ge "$N" ]; then
    echo "TOTAL fails=? / $N  kernel=$PIN  ALREADY COMPLETE  report=$OUT" | tee -a "$OUT"
    exit 0
fi
for i in $(seq $((done_n + 1)) "$N"); do
    start=$(date +%s)
    # boot_test.sh unlinks its SERIAL_LOG on every exit path, so the capture has
    # to be mirrored WHILE the run is live (same pattern as gate_evidence.sh).
    LIVE="build/artifacts/.o10-$TS-$i.live"
    log="build/artifacts/o10-$TS-$i.log"
    rm -f "$LIVE" "$log"
    hash_check before "$i"
    ( while :; do [ -f "$LIVE" ] && cp -f "$LIVE" "$log" 2>/dev/null; sleep 1; done ) &
    poller=$!
    SERIAL_LOG="$LIVE" make smoke-sfs-btree-smp4 > /tmp/o10.log 2>&1
    rc=$?
    sleep 2; kill "$poller" 2>/dev/null; wait "$poller" 2>/dev/null
    hash_check after "$i"
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
# fails counts THIS CHUNK; the report is the accumulator, so recount it.
tot_runs=$(grep -c "^run=" "$OUT" 2>/dev/null || true); [ -z "$tot_runs" ] && tot_runs=0
tot_fail=$(grep -c "verdict=FAIL" "$OUT" 2>/dev/null || true); [ -z "$tot_fail" ] && tot_fail=0
echo "TOTAL fails=$tot_fail / $tot_runs (target $N)  kernel=$PIN  report=$OUT" | tee -a "$OUT"
