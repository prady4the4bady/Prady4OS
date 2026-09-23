#!/usr/bin/env bash
# hunt_print_selftest.sh -- DDR-1135 meta-test for tools/ci/hunt_print.sh.
#
# The printer only runs after a hunt lane has found something, which is to say
# almost never, and only in CI. A defect in it cannot show up in a green run, so
# it is tested here on fixtures with no QEMU.
#
# THE FIXTURE REPRODUCES LANE 12'S GEOMETRY (DDR-1134 s.6, DDR-1135 s.6(b)), not
# just "a capture containing a panic". A fixture whose panic is the only match
# passes on a printer that prints one window around one match. So: ~214 lines of
# boot, the banner, a 33-line body carrying NO SIGNALS token, then a heartbeat
# flood carrying panic_stage=, then [apfreeze] shots. That is 41 matches against
# an index cap of 40.
#
# The arms, and why each one is there:
#   P  precondition: the body contains no SIGNALS token. If it did, the index
#      would print the body and arms A/B would pass with no context at all.
#      This is asserted about the fixture, not assumed (DDR-1135 s.6(a)).
#   A  near: 'component: NEXUS isr', the line right after the banner (+1).
#   B  far: 'halting.' at the end of the report (+32). This is the arm that
#      carries the claim.
#   C  the index says when its cap bit.
#   D  the output is bounded.
#   E  a second fixture, 30 matches 60 lines apart (so over the cap at any
#      window of 5 or more): the context cap names
#      itself when it bites.
#
# MUTANTS. Each has to fail a DIFFERENT set of arms, which is how we know the
# arms are independent (the DDR-1044 check):
#   M1  the pre-fix printer verbatim (grep | head -40). It is a real prior
#       state, not a synthetic defect (DDR-1066/1067 form). Expected to fail
#       A, B, C and E.
#   M2  trailing context shrunk to 5. Expected to fail B only. It prints
#       several plausible-looking report lines, so a check that only asked
#       whether something followed the banner would ship it.
#   M3  leading context only (DDR-1088's original defect). Expected to fail A
#       and B while still printing context, which is why "context was printed"
#       is not an arm.
set -u
cd "$(dirname "$0")/../.." || exit 2
D=build/gatelogs/huntprint
mkdir -p "$D"
SIGNALS='\[ringwalk\]|\[apfreeze\]|\[schedcheck\]|panic_stage=|NEXUS KERNEL PANIC|gs FAIL'
# Fail loudly if this copy of SIGNALS drifts from the campaign's.
grep -qF "SIGNALS='$SIGNALS'" tools/ci/open2_hunt_campaign.sh \
    || { echo "huntprint-selftest FAIL: SIGNALS here differs from open2_hunt_campaign.sh"; exit 1; }

FX="$D/lane12.log"
BODY="$D/body.txt"
{
    echo '*** NEXUS KERNEL PANIC ***'
    echo 'component: NEXUS isr'
    echo 'exception: #PF page fault vector=0xE error=0x0'
    echo 'RIP=0xFFFFFFFF80012A91 CS=0x08'
    echo 'RFLAGS=0x0000000000010046'
    echo 'RSP=0x0000000007FA3E40'
    echo 'CR2=0x00000000DEADBEEF'
    echo 'SS=0x10'
    for r in RAX RBX RCX RDX RSI RDI RBP R8 R9 R10 R11 R12 R13 R14 R15 CR3; do
        echo "$r=0x0000000000000000"
    done
    echo 'backtrace:'
    for f in 1 2 3 4 5 6; do echo "  [$f] 0xFFFFFFFF800160$f"; done
    echo '<frame chain ends: fp=0x0>'
    echo 'halting.'
} > "$BODY"
{
    for i in $(seq 1 214); do echo "[boot] filler line $i"; done
    cat "$BODY"
    for i in $(seq 1 36); do
        echo "[hb] t=$((i*500)) rqcpus=3 ymask=12345 panics_silent=0 panic_stage=3 loser_cpu=0"
        echo "[boot] after hb $i"
    done
    for s in 1 2 3 4; do
        echo "[apfreeze] cpu=3 ticks=155 rip=0xFFFFFFFF80016D91 if=0 shot=$s"
        echo "  bt: 0xFFFFFFFF80016841"
    done
    echo '[boot] tail'
} > "$FX"

FX2="$D/spread.log"
{
    for s in $(seq 1 30); do
        echo "[apfreeze] cpu=3 ticks=155 shot=$s"
        for i in $(seq 1 60); do echo "[boot] gap $s.$i"; done
    done
} > "$FX2"

rc=0
body_lines=$(wc -l < "$BODY")
[ "$body_lines" -eq 33 ] || { echo "huntprint-selftest FAIL: fixture body is $body_lines lines, not 33"; rc=1; }
# P: only the banner itself may match.
bm=$(grep -cE "$SIGNALS" "$BODY")
[ "$bm" -eq 1 ] || { echo "huntprint-selftest FAIL (P): the fixture body carries $bm SIGNALS matches, not 1 -- arms A/B would be vacuous"; rc=1; }
m=$(grep -cE "$SIGNALS" "$FX")
[ "$m" -eq 41 ] || { echo "huntprint-selftest FAIL: the fixture has $m matches, not 41"; rc=1; }

# arms <tag> <outfile> <outfile2> -> prints the failing arm letters
arms() {
    local o="$1" o2="$2" f=""
    grep -qF 'component: NEXUS isr' "$o" || f="${f}A"
    grep -qx '[0-9]*-halting\.' "$o" || f="${f}B"
    grep -q 'index TRUNCATED: 41 matches' "$o" || f="${f}C"
    [ "$(wc -l < "$o")" -le 300 ] || f="${f}D"
    grep -q 'context TRUNCATED at' "$o2" || f="${f}E"
    echo "${f:-none}"
}

run() {  # run <tag> <prelude> -> failing arms
    local tag="$1" pre="$2"
    bash -c ". tools/ci/hunt_print.sh; $pre; hunt_print \"\$1\" \"\$2\"" _ "$FX" "$SIGNALS" > "$D/$tag.out" 2>&1
    bash -c ". tools/ci/hunt_print.sh; $pre; hunt_print \"\$1\" \"\$2\"" _ "$FX2" "$SIGNALS" > "$D/$tag.2.out" 2>&1
    arms "$D/$tag.out" "$D/$tag.2.out"
}

expect() {  # expect <tag> <want> <got>
    if [ "$3" = "$2" ]; then echo "huntprint-selftest: $1 failing-arms=$3 (expected $2) OK"
    else echo "huntprint-selftest FAIL: $1 failing-arms=$3, expected $2"; rc=1; fi
}

expect shipped none "$(run shipped ':')"
expect M1-prefix ABCE "$(run m1 'hunt_print() { grep -nE "$2" "$1" | head -40; }')"
expect M2-after5 B "$(run m2 'HUNT_CTX_AFTER=5')"
expect M3-leadonly AB "$(run m3 'HUNT_CTX_BEFORE=40; HUNT_CTX_AFTER=0')"
# M3 must still have PRINTED context, or it is not the defect it models.
grep -q 'filler line 213' "$D/m3.out" \
    || { echo "huntprint-selftest FAIL: M3 printed no leading context, so it does not model DDR-1088's defect"; rc=1; }

[ "$rc" -eq 0 ] && echo "huntprint-selftest: PASS (shipped passes all arms; M1/M2/M3 each fail their own set)"
exit "$rc"
