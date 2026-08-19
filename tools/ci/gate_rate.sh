#!/usr/bin/env bash
# gate_rate.sh <gate> <N> — run a gate N times and report pass/fail counts.
# S22/R11: a single pass and a single fail on one tip is an INTERMITTENT, and no
# conclusion may be drawn from N=1. This produces the denominator (I23).
# Also counts [sfs-uaf] occurrences per run so the DDR-953 rate is measured, not
# guessed. Hashes the kernel each run and ABORTS if it changes mid-sequence
# (RULE 19: a rebuild partway through means later runs tested a different tree).
cd "$(dirname "$0")/../.." || exit 2
gate="${1:?usage: gate_rate.sh <gate> <N>}"
n="${2:?usage: gate_rate.sh <gate> <N>}"
mkdir -p build/gatelogs
ref=$(md5sum build/kernel.bin | cut -d' ' -f1)
pass=0; fail=0; uaftotal=0; uafruns=0
for i in $(seq 1 "$n"); do
    cur=$(md5sum build/kernel.bin | cut -d' ' -f1)
    if [ "$cur" != "$ref" ]; then
        echo "gate_rate: ABORT — kernel hash changed mid-sequence ($ref -> $cur)"
        echo "gate_rate: results so far are VOID"
        exit 4
    fi
    log="build/gatelogs/rate_${gate}_$i.log"
    make "$gate" > "$log" 2>&1
    rc=$?
    if [ ! -s "$log" ]; then echo "gate_rate: run $i EMPTY LOG — harness failure"; exit 3; fi
    u=$(grep -ac "sfs-uaf" "$log")
    uaftotal=$((uaftotal + u))
    [ "$u" -gt 0 ] && uafruns=$((uafruns + 1))
    if grep -aq "^\[smoke\] PASS" "$log"; then
        pass=$((pass + 1)); verdict=PASS
    else
        fail=$((fail + 1)); verdict=FAIL
    fi
    echo "gate_rate: run $i/$n rc=$rc verdict=$verdict sfs-uaf-lines=$u"
done
echo "gate_rate: ==== $gate over N=$n ===="
echo "gate_rate: PASS=$pass FAIL=$fail"
echo "gate_rate: runs with sfs-uaf=$uafruns  total uaf lines=$uaftotal"
echo "gate_rate: kernel md5=$ref (unchanged across all runs)"
