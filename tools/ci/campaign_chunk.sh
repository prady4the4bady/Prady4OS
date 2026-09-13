#!/usr/bin/env bash
# tools/ci/campaign_chunk.sh — RESUMABLE campaign runner, for environments that
# reap detached processes between turns.
#
# WHY THIS EXISTS, and why campaign.sh is not enough here.
#
# campaign.sh solves a real problem — campaigns dying when the launching shell
# is torn down — with `setsid` + full detach. That works on a local WSL shell.
# It does NOT work in this remote container: a 60-run campaign started with it
# was found dead at 2/60 seventeen minutes later, its status file still reading
# `state=running` (so it was killed, not exited) and its run-2 log truncated
# right after the boot banner. Detaching does not help when the whole process
# group is reaped between turns.
#
# The fix is not a better detach — it is to stop needing one. This script does a
# BOUNDED chunk of work and exits cleanly, and is resumable because it derives
# progress from the logs on disk rather than from a live process. Run it again
# and it picks up where it stopped, however it stopped.
#
#   usage: campaign_chunk.sh <gate> <target-N> [budget-seconds] [serial-log]
#
# SERIAL-LOG SNAPSHOT (measured, DDR-1000 §10). The per-run file this script
# keeps is `make` OUTPUT, and make output contains NONE of the guest's serial
# capture — verified: 0 lines matching `[smoke]` or `[hb]` in a completed run.
# Scanning those files for a guest-side sentinel like `[yieldstall]` therefore
# cannot find one even when it is there, which made a 60-run scan vacuous.
# Pass the gate's serial path (e.g. build/evresize.log) and each run's capture
# is snapshotted to <gate>.<i>.serial.log before the next run overwrites it.
#
# It refuses to start while any QEMU is live (§NON-NEGOTIABLE 12, bracket form
# per §INV.3), and it records the kernel hash of every run so a campaign that
# spans a rebuild is DETECTABLE rather than silently mixed (R1) — that mistake
# cost four runs earlier in this session.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gate="${1:?usage: campaign_chunk.sh <gate> <target-N> [budget-seconds]}"
target="${2:?target N}"
budget="${3:-480}"                       # default 8 min: fits a 10 min ceiling
serial="${4:-}"                          # gate's serial capture, snapshot per run

LOGDIR="$ROOT/build/gatelogs/campaign"
# DDR-1002: a two-arm campaign runs the SAME gate on two different kernels, so
# the ledger cannot be keyed on the gate name alone — arm B would append to arm
# A's ledger and the single-hash check would then fire on a mix that is correct
# by design. CAMPAIGN_TAG separates them; unset keeps the historical filename.
tag="${CAMPAIGN_TAG:-}"
LEDGER="$LOGDIR/$gate${tag:+.$tag}.ledger.txt"   # one line per completed run
mkdir -p "$LOGDIR"

if pgrep -f "[q]emu-system-x86_64" >/dev/null; then
    echo "campaign_chunk: REFUSING — a QEMU is already live (§NON-NEGOTIABLE 12)"
    exit 2
fi

done_n=$(grep -c . "$LEDGER" 2>/dev/null || true)
[ -z "$done_n" ] && done_n=0
echo "campaign_chunk: $gate — $done_n/$target already done, budget ${budget}s"
if [ "$done_n" -ge "$target" ]; then
    echo "campaign_chunk: COMPLETE"
    exit 0
fi

deadline=$(( $(date +%s) + budget ))
while [ "$done_n" -lt "$target" ]; do
    now=$(date +%s)
    # Start a run only if the whole run plausibly fits; a run cut off midway is
    # not a data point, and counting it would inflate N with a non-observation.
    if [ "$now" -ge "$deadline" ]; then
        echo "campaign_chunk: budget spent at $done_n/$target — rerun to continue"
        break
    fi
    i=$(( done_n + 1 ))
    kh=$(sha256sum "$ROOT/build/kernel.bin" 2>/dev/null | cut -c1-16)
    if make -C "$ROOT" "$gate" >"$LOGDIR/$gate${tag:+.$tag}.$i.log" 2>&1; then
        verdict=PASS
    else
        verdict=FAIL
    fi
    # Hash AFTER the run too: `make` may rebuild, and a campaign that silently
    # spans two kernels is not a campaign.
    kh2=$(sha256sum "$ROOT/build/kernel.bin" 2>/dev/null | cut -c1-16)
    # Snapshot the guest capture BEFORE the next run truncates it. Without this
    # the only per-run artefact is make output, which carries no serial lines.
    #
    # DDR-1010 §6 root-caused here: this used to test `[ -f "$ROOT/$serial" ]`
    # unconditionally, so an ABSOLUTE serial path became
    # "/home/user/Prady4OS//home/user/Prady4OS/build/..." -- which never exists,
    # so the copy silently did nothing and every capture was lost. That is
    # exactly how 17 of 20 runs in the smoke-smppreempt campaign came back with
    # no serial log: the first chunk was invoked with a RELATIVE path and kept
    # its captures, the resumed chunk used an absolute one and kept none.
    # Accept both, and SAY SO when the path resolves to nothing rather than
    # dropping the capture quietly -- a silent no-op here is indistinguishable
    # from a clean run with nothing to record.
    case "$serial" in
        /*) __src="$serial" ;;
        "") __src="" ;;
        *)  __src="$ROOT/$serial" ;;
    esac
    if [ -n "$__src" ]; then
        if [ -f "$__src" ]; then
            cp "$__src" "$LOGDIR/$gate${tag:+.$tag}.$i.serial.log" 2>/dev/null || true
        else
            echo "campaign_chunk: WARNING run $i — no capture at $__src;" \
                 "this run has NO serial log (DDR-1010 §6)"
        fi
    fi
    printf '%d\t%s\t%s\t%s\t%s\n' "$i" "$verdict" "$kh" "$kh2" "$(date -u +%FT%TZ)" \
        >>"$LEDGER"
    done_n=$i
    echo "campaign_chunk: run $i/$target $verdict kernel=$kh${kh2:+/$kh2}"
done

p=$(awk -F'\t' '$2=="PASS"' "$LEDGER" 2>/dev/null | grep -c . || true)
f=$(awk -F'\t' '$2=="FAIL"' "$LEDGER" 2>/dev/null | grep -c . || true)
k=$(awk -F'\t' '{print $3"\n"$4}' "$LEDGER" 2>/dev/null | sort -u | grep -c . || true)
echo "campaign_chunk: $gate now $done_n/$target — pass=$p fail=$f distinct_kernels=$k"
[ "$k" -le 1 ] || echo "campaign_chunk: WARNING — $k distinct kernels in the ledger; this is NOT one campaign"
exit 0
