#!/usr/bin/env bash
# sym_bt.sh <logfile> — resolve every backtrace address in a panic log.
# Uses sym_at.py (exact 64-bit integers). awk/shell arithmetic on kernel
# addresses is wrong above 2^53 (S21/I28).
cd "$(dirname "$0")/../.." || exit 2
log="${1:?usage: sym_bt.sh <log>}"
[ -s "$log" ] || { echo "sym_bt: EMPTY/MISSING $log"; exit 3; }
grep -aoE "0xFFFFFFFF[0-9A-Fa-f]{8}" "$log" | awk '!seen[$0]++' | while read -r a; do
    python3 tools/ci/sym_at.py "$a" 2>/dev/null | head -1
done
