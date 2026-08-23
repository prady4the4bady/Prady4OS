#!/usr/bin/env bash
# tools/ci/run_shard.sh <shard-index> — DDR-817. Run one shard's gates.
#
# Replaces 111 hand-written YAML steps. That listing was not just verbose: a
# gate could be dropped from it by an ordinary editing mistake and nothing
# noticed — which is how eight gates came to be in the Makefile and in no CI
# run. `make ci-shard-check` is the assertion; this is the runner.
#
# Per-gate attribution is preserved deliberately. A bare loop would report only
# "the shard failed"; each gate is wrapped in a ::group:: and a failure prints
# the target name and stops immediately, so a red still names the gate.
set -uo pipefail

SHARD="${1:?usage: run_shard.sh <shard-index>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="$ROOT/tools/ci/gate_shards.txt"

mapfile -t GATES < <(awk -F'\t' -v s="$SHARD" '/^[0-9]/ && NF>=3 && $1==s { print $2 }' "$MANIFEST")

if [ "${#GATES[@]}" -eq 0 ]; then
    echo "run_shard: FAIL — shard $SHARD has no gates. A shard index with no" >&2
    echo "           work is a packing bug, not an empty success." >&2
    exit 1
fi

echo "shard $SHARD: ${#GATES[@]} gates"
failed=""
count=0

for g in "${GATES[@]}"; do
    count=$((count + 1))
    echo "::group::[$count/${#GATES[@]}] make $g"
    start=$SECONDS
    # DDR-979: merge make's stderr into the SAME file description as its stdout.
    # Without this the two are separate fds into one pipe with no ordering
    # guarantee, and a failing gate's serial dump interleaves MID-LINE with
    # make's own error. That is how the only capture of the intermittent ring-0
    # panic was destroyed at capture time:
    #
    #   component: NEXUS isr
    #   make: *** [Makefile:2307: smoke-blk-integrity] Error 1
    #   exception: make: Leaving directory '...'
    #
    # -- the "exception: <name> vector= error= RIP=" block, i.e. the only lines
    # that identify the fault, overwritten by make's. Costs nothing and makes the
    # next occurrence diagnosable.
    if make -C "$ROOT" "$g" 2>&1; then
        echo "PASS $g ($((SECONDS - start))s)"
    else
        rc=$?
        echo "::endgroup::"
        echo "::error title=gate failed::$g (shard $SHARD) exited $rc"
        failed="$g"
        break
    fi
    echo "::endgroup::"
done

if [ -n "$failed" ]; then
    echo "shard $SHARD: FAILED at $failed after $count of ${#GATES[@]} gates"
    exit 1
fi

# Printed so the sum across shards can be checked against the gate count the
# single serial job used to run — a shard that quietly ran fewer would
# otherwise be invisible.
echo "shard $SHARD: ALL PASS — ${#GATES[@]} gates"
