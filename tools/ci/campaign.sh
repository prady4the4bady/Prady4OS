#!/usr/bin/env bash
# tools/ci/campaign.sh — persistent N-run gate campaign (operator directive §6.1).
#
# WHY THIS EXISTS. Long verification campaigns kept dying when the shell that
# launched them was torn down, silently restarting from run 1 and costing 30+
# minutes each time. That happened again in this session: a 20-run
# smoke-surfdestroy campaign was still on run 2 after 15 minutes with no way to
# see progress except by counting log files.
#
# Two properties fix that:
#   1. setsid + full detach, so the campaign outlives its invoker;
#   2. a status file rewritten EVERY iteration, so progress is pollable with
#      `cat build/campaign_status.txt` and never requires a foreground shell.
#
# Usage:
#   tools/ci/campaign.sh start <gate> <count>   # detach and run
#   tools/ci/campaign.sh status                 # print progress, exit 0
#   tools/ci/campaign.sh wait                   # block until done (for scripts)
#   tools/ci/campaign.sh stop                   # kill a running campaign
#
# Honours §NON-NEGOTIABLE 12: refuses to start while any QEMU is live, using the
# bracket form from §INV.3 (`pgrep qemu-system-x86_64` matches nothing — the
# comm field is truncated to 15 chars).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATUS="$ROOT/build/campaign_status.txt"
LOGDIR="$ROOT/build/gatelogs/campaign"
PIDFILE="$ROOT/build/campaign.pid"

qemu_live() { pgrep -f "[q]emu-system-x86_64" >/dev/null; }

run_campaign() {
    local gate="$1" count="$2" pass=0 fail=0 i
    mkdir -p "$LOGDIR"
    local khash
    khash="$(sha256sum "$ROOT/build/kernel.bin" 2>/dev/null | cut -d' ' -f1)"
    for ((i=1; i<=count; i++)); do
        {   echo "gate=$gate"
            echo "run=$i/$count"
            echo "pass=$pass fail=$fail"
            echo "kernel=$khash"          # R1: hash with every measurement
            echo "pid=$$"
            echo "state=running"
            echo "updated=$(date -u +%FT%TZ)"
        } > "$STATUS"
        if make -C "$ROOT" "$gate" >"$LOGDIR/$gate.$i.log" 2>&1; then
            pass=$((pass+1))
        else
            fail=$((fail+1))
            # Classify immediately (§6.2) so a known intermittent is named in
            # the status file rather than re-diagnosed later from scratch.
            "$ROOT/tools/ci/classify_failure.sh" "$LOGDIR/$gate.$i.log" \
                >>"$LOGDIR/$gate.classify.txt" 2>&1 || true
        fi
    done
    {   echo "gate=$gate"
        echo "run=$count/$count"
        echo "pass=$pass fail=$fail"
        echo "kernel=$khash"
        echo "pid=$$"
        echo "state=done"
        echo "updated=$(date -u +%FT%TZ)"
    } > "$STATUS"
    rm -f "$PIDFILE"
}

case "${1:-}" in
start)
    gate="${2:?usage: campaign.sh start <gate> <count>}"
    count="${3:?usage: campaign.sh start <gate> <count>}"
    if qemu_live; then
        echo "campaign: refusing to start — QEMU already running (§NON-NEGOTIABLE 12)" >&2
        exit 1
    fi
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "campaign: one is already running (pid $(cat "$PIDFILE"))" >&2
        exit 1
    fi
    mkdir -p "$ROOT/build"
    # setsid detaches into a new session, so the campaign survives its invoker
    # exiting — the exact failure mode §6.1 was written about.
    setsid bash "$0" __run "$gate" "$count" </dev/null >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    echo "campaign: started $gate x$count (pid $(cat "$PIDFILE")); poll: cat build/campaign_status.txt"
    ;;
__run) run_campaign "$2" "$3" ;;
status)
    [ -f "$STATUS" ] && cat "$STATUS" || echo "state=none"
    ;;
wait)
    while [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; do sleep 10; done
    cat "$STATUS" 2>/dev/null
    ;;
stop)
    if [ -f "$PIDFILE" ]; then kill -- "-$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"; fi
    pkill -f "[q]emu-system-x86_64" 2>/dev/null
    echo "campaign: stopped"
    ;;
*)  sed -n '4,26p' "$0"; exit 1 ;;
esac
