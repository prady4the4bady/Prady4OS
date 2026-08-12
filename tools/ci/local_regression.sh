#!/usr/bin/env bash
# local_regression.sh — run EXACTLY the gate set CI runs, derived from
# tools/ci/gate_shards.txt. Never a hand-picked list.
#
# WHY THIS EXISTS
#
# "18/18 green locally" was reported three times in one session while CI was red.
# Each time the number was true and meaningless: the 18 were chosen by hand and
# did not include the gates that were failing. `make ci-shard-check` guarantees
# every Makefile gate is ASSIGNED to a shard; nothing guaranteed the gates run
# locally matched the gates CI executes. This closes that gap by construction —
# the list comes from the same file the CI matrix reads.
#
# USAGE
#   bash tools/ci/local_regression.sh                 # every gate CI runs
#   bash tools/ci/local_regression.sh --shard 0       # one shard
#   bash tools/ci/local_regression.sh --list          # print the set, run nothing
#
# A full run is ~143 gates and takes hours. --shard exists so a machine can cover
# CI in pieces. Whatever subset is run, this script PRINTS the coverage fraction,
# so a partial run can never be mistaken for a CI-equivalent one.
set -u
cd "$(dirname "$0")/../.." || exit 1

SHARDS_FILE=tools/ci/gate_shards.txt
[ -r "$SHARDS_FILE" ] || { echo "local-regression: cannot read $SHARDS_FILE"; exit 1; }

want_shard=""
list_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --shard) want_shard="${2:-}"; shift 2 ;;
        --list)  list_only=1; shift ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done

# Column 1 = shard, column 2 = make target. Comment lines are excluded, which is
# also how excluded gates (leading '#excluded') stay out.
total=$(awk '!/^#/ && NF>=2 {n++} END{print n+0}' "$SHARDS_FILE")
if [ -n "$want_shard" ]; then
    gates=$(awk -v s="$want_shard" '!/^#/ && NF>=2 && $1==s {print $2}' "$SHARDS_FILE")
else
    gates=$(awk '!/^#/ && NF>=2 {print $2}' "$SHARDS_FILE")
fi

count=$(printf '%s\n' "$gates" | grep -c . || true)
[ "$count" -gt 0 ] || { echo "local-regression: no gates selected — check --shard"; exit 1; }

if [ "$list_only" -eq 1 ]; then
    printf '%s\n' "$gates"
    echo "--- $count of $total CI gates ---"
    exit 0
fi

# Build freshness is checked BEFORE building, which is the only point at which it
# means anything: afterwards make is legitimately up to date and reports a false
# alarm. See DDR-912.
bash tools/ci/build_freshness.sh --fix || exit 1

mkdir -p diag
fail=0; ran=0; failed=""
for g in $gates; do
    if [ -z "$g" ]; then echo "EMPTY GATE NAME — abort"; exit 8; fi
    grep -q "^$g:" Makefile || { echo "SKIP $g (target absent)"; continue; }
    ran=$((ran+1))
    if timeout 600 make "$g" >"diag/lr.$g.log" 2>&1; then
        echo "PASS $g"
    else
        echo "FAIL $g"
        grep -aE "FAIL|error:|Error [0-9]" "diag/lr.$g.log" | tail -2
        fail=1; failed="$failed $g"
    fi
done

echo "=== local-regression: ran $ran of $total CI gates ($((ran * 100 / total))% coverage) ==="
[ -n "$failed" ] && echo "FAILED:$failed"
if [ "$ran" -lt "$total" ]; then
    echo "NOTE: this is a PARTIAL run. It is not equivalent to CI green."
fi
exit "$fail"
