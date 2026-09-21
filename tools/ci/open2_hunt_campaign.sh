#!/usr/bin/env bash
# DDR-1096 — OPEN-2 hunting campaign (delay-injection variant).
#
# Runs the SMP boot N times on ONE pinned kernel and scans each capture for the
# four OPEN-2 signals. Written to the disciplines this project has already paid
# for:
#
#   * DDR-1060 §9 — the kernel hash is PINNED and verified BEFORE AND AFTER every
#     run, and the campaign ABORTS rather than warns. A campaign that rebuilt
#     mid-flight has already been voided once here.
#   * DDR-1023 — each run gets its OWN SERIAL_LOG, and the capture is asserted to
#     contain boot output before it is scanned. A previous campaign scanned make
#     output (3010 B, zero [hb] lines), so its grep was vacuous.
#   * DDR-1097 §7.2 — and a capture WITH boot output can still be vacuous. The
#     race needs thread create/exit churn, which on a gate boot comes from
#     rqstress_proof; the hunt pauses slow the boot, and MEASURED 1 run in 3 at
#     the working point never reached it inside the window. Heartbeats kept
#     arriving, so nothing looked wrong — the run simply never exercised the
#     window and was scored `clean`. Runs are now classified, and a lane where
#     NO run had churn is reporting on an experiment that did not happen.
#   * §NON-NEGOTIABLE 12 — one QEMU at a time; the bracket-form pre-flight also
#     has to tolerate this script's own argv (see §INV.3 refinement in the DDR).
#
# Usage: OPEN2_HUNT=<n> bash tools/ci/open2_hunt_campaign.sh <runs>
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
N="${1:-10}"
OUT="build/gatelogs/open2hunt"
mkdir -p "$OUT"

PIN="$(sha256sum build/kernel.bin | cut -d' ' -f1)"
echo "[campaign] kernel_pinned=${PIN:0:16} runs=$N smp=4"

# DDR-1130 — ASSERT THE DATA-DISK PRECONDITION BEFORE RUN 1.
#
# Without these the guest gets ONE drive (build/pradyos.img, an MBR boot image
# that is not mountable), so fs_test_thread RETURNS at main.c:1414-1417 and
# rqstress_proof -- unconditional, ~1,500 lines below, after [boot-stamp] B --
# is UNREACHABLE. Every run then reports NO-CHURN, deterministically, forever:
# a wide window with nothing to catch in it, which is DDR-1097 §7.2's hazard
# arriving from a second and entirely different cause. Measured: 2/2 runs, zero
# [boot-stamp], zero rqstress, and 35/35 heartbeats at ymask=0 -- DDR-1096
# §4.3's own signature for a kernel that cannot mount its filesystem, which is
# the trap that DDR refused a harness over.
#
# The gates get these as MAKE prerequisites (`smoke-rqstress: $(IMG) fat-image
# sfs-image`) and open2-hunt.yml builds them in its own step, so CI was never
# exposed -- which is why DDR-1127 measured 60/60 with churn. A bare invocation
# of this script has no prerequisites at all, and had no assertion either: the
# binary precondition is asserted (PIN, above, DDR-1060 §9) and this one was not.
#
# ASSERT, DO NOT BUILD (DDR-1130 §6). Invoking make here would let the pin move
# mid-campaign, which is precisely what VOIDED a campaign in DDR-1060 §9.
missing=""
for img in build/fat.img build/sfs.img; do
    [ -f "$img" ] || missing="$missing $img"
done
if [ -n "$missing" ]; then
    echo "[campaign] ABORT before run 1: missing data disk(s):$missing"
    echo "[campaign]   Without them the guest has no mountable filesystem, so"
    echo "[campaign]   rqstress_proof never runs and EVERY run reports NO-CHURN."
    echo "[campaign]   That is not a clean result; it is not an experiment."
    echo "[campaign]   Build them first (this script deliberately will not):"
    echo "[campaign]       make fat-image sfs-image"
    echo "[campaign]   then re-check the pin, because those targets can rebuild"
    echo "[campaign]   the kernel (DDR-1060 §9): sha256sum build/kernel.bin"
    exit 2
fi

# DDR-1129: `[schedcheck]` was MISSING here, and it is the line that NAMES THE
# MECHANISM. DDR-1128's capture halted at sched.c:1857 -- DDR-1105's next->rsp
# validity check -- which emits `[schedcheck] next->rsp invalid ... halting.`
# carrying DDR-1118's rq_on/disp/saves discriminators, and THEN halts. That line
# sat ONE LINE ABOVE the first `[apfreeze]` and was never printed: the job log
# carried four copies of the symptom and zero of the reason. `[schedcheck]` has
# been in GLOBAL_FORBIDDEN since DDR-1105, so it was load-bearing everywhere but
# here. Adding it also WIDENS detection: a BSP-side halt emits `[schedcheck]`
# with no `[apfreeze]` at all, and would have read as `clean`.
SIGNALS='\[ringwalk\]|\[apfreeze\]|\[schedcheck\]|panic_stage=|NEXUS KERNEL PANIC|gs FAIL'
# The unlink churn the race requires. rqstress_proof spawns and exits a 24-thread
# burst; without it the boot ran with a wide window and nothing to catch in it.
CHURN='\[smp\] rqstress OK'
hits=0
churn=0
for i in $(seq 1 "$N"); do
    now="$(sha256sum build/kernel.bin | cut -d' ' -f1)"
    if [ "$now" != "$PIN" ]; then
        echo "[campaign] ABORT run $i: kernel.bin changed (${now:0:16} != ${PIN:0:16})"; exit 2
    fi
    log="$OUT/run-$i.log"
    rm -f "$log" "$log".fail-*
    TIMEOUT_S=180 QEMU_SMP=4 KEEP_SERIAL=1 SERIAL_LOG="$log" \
        FORBIDDEN_SENTINEL="__never_appears__" \
        bash tools/qemu_runner/boot_test.sh build/pradyos.img >/dev/null 2>&1
    rc=$?
    # boot_test renames the capture on failure; take whichever exists.
    cap="$(ls -t "$log" "$log".fail-* 2>/dev/null | head -1)"

    after="$(sha256sum build/kernel.bin | cut -d' ' -f1)"
    if [ "$after" != "$PIN" ]; then
        echo "[campaign] ABORT after run $i: kernel.bin changed mid-campaign"; exit 2
    fi

    # DDR-1023: a capture with no boot output makes the scan below vacuous.
    if [ -z "$cap" ] || ! grep -q '\[hb\]' "$cap" 2>/dev/null; then
        echo "[campaign] run=$i rc=$rc VACUOUS-CAPTURE cap=${cap:-none}"; continue
    fi

    hb="$(grep -o '^\[hb\] t=[0-9]*' "$cap" | tail -1)"
    sig="$(grep -cE "$SIGNALS" "$cap")"
    if grep -qE "$CHURN" "$cap"; then churn=$((churn+1)); ch=churn; else ch=NO-CHURN; fi
    if [ "$sig" -gt 0 ]; then
        hits=$((hits+1))
        echo "[campaign] run=$i rc=$rc ${hb} $ch *** SIGNAL x$sig *** cap=$cap"
        # DDR-1129: was `head -5`. DDR-1128's run produced 4 `[apfreeze]` shots
        # plus 1 `[schedcheck]`, i.e. EXACTLY 5 -- one more shot and the cap
        # would have silently dropped a line. The whole point of this print is
        # that the diagnosis and the symptom travel together.
        grep -nE "$SIGNALS" "$cap" | head -40
    else
        echo "[campaign] run=$i rc=$rc ${hb} $ch clean"
    fi
done
# churn_runs is the DENOMINATOR (NON-NEGOTIABLE 17): a bound computed over runs
# that never ran the churn is a bound over boots that could not have fired.
echo "[campaign] DONE runs=$N signal_runs=$hits churn_runs=$churn kernel_pinned=${PIN:0:16}"
