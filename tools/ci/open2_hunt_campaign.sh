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

SIGNALS='\[ringwalk\]|\[apfreeze\]|panic_stage=|NEXUS KERNEL PANIC|gs FAIL'
hits=0
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
    if [ "$sig" -gt 0 ]; then
        hits=$((hits+1))
        echo "[campaign] run=$i rc=$rc ${hb} *** SIGNAL x$sig *** cap=$cap"
        grep -nE "$SIGNALS" "$cap" | head -5
    else
        echo "[campaign] run=$i rc=$rc ${hb} clean"
    fi
done
echo "[campaign] DONE runs=$N signal_runs=$hits kernel_pinned=${PIN:0:16}"
