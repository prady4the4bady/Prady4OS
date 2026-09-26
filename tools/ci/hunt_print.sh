# hunt_print.sh -- the OPEN-2 hunt's signal printer (DDR-1135). SOURCED, not run.
#
# Extracted from open2_hunt_campaign.sh so it can be tested on a fixture without
# booting anything (tools/ci/hunt_print_selftest.sh). The campaign sources this
# file; there is exactly ONE copy of the printer, because DDR-1088 fixed two
# copies of the same printer in one commit precisely because copies drift, and
# the hunt turned out to be the third.
#
# THE DEFECT (DDR-1134 s.6): the printer was `grep -nE "$SIGNALS" "$cap" | head -40`
# -- MATCHING LINES ONLY. A panic is written SUMMARY-FIRST, so the report was
# represented by its banner, the one line that carries no information, and
# component:/exception:/RIP=/backtrace/halting. never reached the job log.
#
# THE DESIGN (DDR-1135 s.4/s.5):
#   1. DETECTION DOES NOT MOVE. The caller still counts with grep -cE "$SIGNALS";
#      nothing here decides whether a run is a signal.
#   2. THE INDEX IS KEPT VERBATIM -- every match, heartbeats included, capped at
#      40 -- and now SAYS when the cap bit, instead of stopping silently (lane 12
#      had exactly 40 matches against a cap of 40: zero margin).
#   3. ADDED: a merged context window, LEADING AND TRAILING, around every match
#      that is NOT an [hb] heartbeat. Heartbeats match because panic_stage= is a
#      heartbeat field (DDR-1049, correct), and in lane 12 they took 35 of 40
#      index slots; surrounding each with context would spend the budget on
#      lines that are not events. Excluding them from CONTEXT cannot hide
#      anything: the unfiltered index above still prints them. This is a
#      statement about one line's shape, which this script already depends on
#      twice, NOT the cause/symptom ranking DDR-1079 refused.
#   4. BOUNDED, and the bound names itself when it bites.
#
# Trailing 40 is sized by measurement, not taste: DDR-1088 measured a real panic
# report at 33 lines (banner to halting.) with 6 frames, and the backtrace walker
# is bounded at 8 frames, so ~35 lines worst case.
HUNT_IDX_MAX="${HUNT_IDX_MAX:-40}"
HUNT_CTX_BEFORE="${HUNT_CTX_BEFORE:-5}"
HUNT_CTX_AFTER="${HUNT_CTX_AFTER:-40}"
HUNT_CTX_MAX="${HUNT_CTX_MAX:-240}"

# hunt_print <capture> <signals-ERE>
hunt_print() {
    local cap="$1" sig="$2" n
    n="$(grep -cE "$sig" "$cap")"
    echo "[campaign]   --- index: $n matching line(s) ---"
    grep -nE "$sig" "$cap" | head -"$HUNT_IDX_MAX"
    if [ "$n" -gt "$HUNT_IDX_MAX" ]; then
        echo "[campaign]   index TRUNCATED: $n matches, first $HUNT_IDX_MAX shown (full capture is in the lane artifact)"
    fi
    # Line numbers of non-heartbeat matches. Heartbeats start the line with
    # [hb], exactly the shape the caller's own grep -o '^\[hb\] t=' relies on.
    local nums
    nums="$(grep -nE "$sig" "$cap" | grep -v '^[0-9]*:\[hb\]' | cut -d: -f1 | tr '\n' ' ')"
    [ -z "$nums" ] && return 0
    echo "[campaign]   --- context: ${HUNT_CTX_BEFORE} before / ${HUNT_CTX_AFTER} after each non-[hb] match ---"
    # POSIX awk only (mawk here -- DDR-1079/1121: no gawk extensions).
    awk -v nums="$nums" -v B="$HUNT_CTX_BEFORE" -v A="$HUNT_CTX_AFTER" -v MAX="$HUNT_CTX_MAX" '
        BEGIN {
            k = split(nums, m, " ")
            for (i = 1; i <= k; i++) {
                lo = m[i] - B; if (lo < 1) lo = 1
                for (j = lo; j <= m[i] + A; j++) keep[j] = 1
            }
            last = 0; out = 0; cut = 0
        }
        (NR in keep) {
            if (out >= MAX) { cut = 1; exit }
            if (last && NR != last + 1) print "--"
            printf "%d-%s\n", NR, $0
            last = NR; out++
        }
        END {
            if (cut) printf "[campaign]   context TRUNCATED at %d lines (full capture is in the lane artifact)\n", MAX
        }' "$cap"
}
