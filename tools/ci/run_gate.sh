#!/usr/bin/env bash
# run_gate.sh <gate> [logname] — run one smoke gate and report a TRUSTWORTHY verdict.
#
# T4/RULE 24: written as an inline `make X > log; echo rc=$?; wc -l < log`
# through `wsl bash -c`, the $(...) and $? are eaten and the redirect target can
# end up unresolved — producing "rc=0" next to "No such file or directory" for
# the very file it claims to have written. Measurement must live in a file.
#
# T3/R3: an empty or missing log is NOT "sentinel not found" — it is a harness
# failure, and it exits 3 so it can never be mistaken for a gate verdict.
cd "$(dirname "$0")/../.." || exit 2
gate="${1:?usage: run_gate.sh <gate> [logname]}"
log="build/gatelogs/${2:-$gate}.log"
mkdir -p build/gatelogs

if command -v pgrep >/dev/null 2>&1 && pgrep -f "[q]emu-system-x86_64" >/dev/null 2>&1; then
    echo "run_gate: STRAY QEMU present — refusing to run (would race the image lock)"
    exit 3
fi

make "$gate" > "$log" 2>&1
rc=$?

if [ ! -s "$log" ]; then
    echo "run_gate: EMPTY/MISSING LOG ($log) — harness failure, NOT a gate verdict"
    exit 3
fi

lines=$(wc -l < "$log")
echo "run_gate: gate=$gate rc=$rc loglines=$lines log=$log"
echo "run_gate: --- verdict lines ---"
grep -aE "^PASS |^\[smoke\] (PASS|FAIL)|^FAIL|required pattern|forbidden" "$log" | head -8
uaf=$(grep -ac "sfs-uaf" "$log")
echo "run_gate: sfs-uaf lines: $uaf"
exit "$rc"
