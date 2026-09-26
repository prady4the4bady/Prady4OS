#!/usr/bin/env bash
# DDR-1151: forced double-dispatch signature catalogue campaign.
# usage: dd_campaign.sh <pinned-hash16> <runs> <gate>... (env DDFLAGS passed to make)
set -u
PIN="$1"; RUNS="$2"; shift 2
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
OUT=build/gatelogs/dd; mkdir -p "$OUT"
for g in "$@"; do
  for i in $(seq 1 "$RUNS"); do
    if pgrep -f "[q]emu-system-x86_64" >/dev/null; then echo "[dd] ABORT stray qemu"; exit 2; fi
    log="$ROOT/$OUT/$g-$i.log"; rm -f "$log" "$log".fail-*
    SERIAL_LOG="$log" KEEP_SERIAL=1 make $DDFLAGS "$g" > "$OUT/$g-$i.make" 2>&1; rc=$?
    h=$(sha256sum build/kernel.bin | cut -c1-16)
    if [ "$h" != "$PIN" ]; then echo "[dd] ABORT kernel hash moved $h != $PIN"; exit 2; fi
    cap=$(ls "$log" "$log".fail-* 2>/dev/null | head -1)
    if [ -z "$cap" ] || [ ! -s "$cap" ]; then echo "[dd] $g run=$i rc=$rc MEASUREMENT-BROKEN (no capture)"; continue; fi
    sc=$(grep -c '\[schedcheck\]' "$cap"); af=$(grep -c '\[apfreeze\]' "$cap")
    pn=$(grep -c 'NEXUS KERNEL PANIC' "$cap"); ps=$(grep -c 'panic_stage=' "$cap")
    vb=$(grep -c 'compl wait timeout' "$cap")
    ddf=$(grep -o 'ddforced=[0-9]*' "$cap" | tail -1); dbl=$(grep -o 'dblclaim=[0-9]*' "$cap" | tail -1)
    lines=$(wc -l < "$cap"); last=$(grep -v '^\s*$' "$cap" | tail -1 | cut -c1-90)
    echo "[dd] $g run=$i rc=$rc lines=$lines sched=$sc apfz=$af panic=$pn pstage=$ps vblk=$vb $ddf $dbl last=\"$last\""
  done
done
echo "[dd] DONE pin=$PIN"
