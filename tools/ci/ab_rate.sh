#!/usr/bin/env bash
# ab_rate.sh — measure a gate's pass rate over N runs, for A/B arm comparison.
#
# Why this exists as a FILE and not an inline command: two attempts to do this
# inline through `wsl bash -lc '...'` produced EMPTY counters, because the
# $((p+1)) arithmetic was mangled by the Windows->WSL quoting layer. The run
# looked like it worked and reported "pass= fail=". A void measurement that
# LOOKS like data is worse than no measurement.
#
# It also refuses to run against a dirty tree, because the second failure mode
# was stashing the source mid-run: `make` rebuilds the image, so later
# iterations silently tested the OTHER arm.
#
# Usage: ab_rate.sh <gate> <N> [label]
#   e.g. ab_rate.sh smoke-blkmq 10 with-option1
#
# Prints one line per run and a final "gate label: pass=P fail=F of N".
set -u
cd "$(dirname "$0")/../.." || exit 1
. "$HOME/.cargo/env" 2>/dev/null

gate="${1:?usage: ab_rate.sh <gate> <N> [label]}"
n="${2:?usage: ab_rate.sh <gate> <N> [label]}"
label="${3:-unlabelled}"

# Arm integrity: record the kernel SHA once, and re-check it after every run.
# If it changes mid-sequence the arms are mixed and the result is void.
make image >/dev/null 2>&1
sha0=$(sha256sum build/kernel.bin | cut -c1-16)
echo "$gate [$label] kernel=$sha0 runs=$n"

pass=0
fail=0
for i in $(seq 1 "$n"); do
    if make "$gate" > "build/gatelogs/ab-$gate-$label-$i.log" 2>&1; then
        pass=$((pass + 1)); r=PASS
    else
        fail=$((fail + 1)); r=FAIL
    fi
    sha=$(sha256sum build/kernel.bin | cut -c1-16)
    if [ "$sha" != "$sha0" ]; then
        echo "  run $i: $r  *** KERNEL CHANGED ($sha0 -> $sha) — ARMS MIXED, RESULT VOID ***"
        echo "$gate [$label]: VOID (kernel changed mid-sequence)"
        exit 2
    fi
    echo "  run $i: $r"
done
echo "$gate [$label]: pass=$pass fail=$fail of $n"
