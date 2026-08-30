#!/usr/bin/env bash
# scan_forbidden.sh <serial-log> [label] — apply GLOBAL_FORBIDDEN to a capture
# produced by a gate that does NOT run through boot_test.sh.
#
# WHY THIS EXISTS (DDR-1010 §8). smoke-shell is one of the eight mandatory
# pre-commit hygiene gates, and it drives QEMU itself through a FIFO rather than
# calling boot_test.sh. It therefore applied NONE of the 73 global sentinels: a
# boot could print [apfreeze], *** NEXUS KERNEL PANIC ***, or [percpu] gs FAIL
# and smoke-shell would still report PASS. Measured, not hypothesised: a kernel
# deliberately mutated to print the gs-FAIL line passed smoke-shell 5/5.
#
# THE EXTRACTION IS FAIL-LOUD, and that is the point. §NON-NEGOTIABLE 6 records
# that GLOBAL_FORBIDDEN was silently EMPTY for four commits and nothing noticed,
# because an empty list fails nothing. The same hazard applies to reading the
# list back out: the sed range ends at the LAST entry, so appending to the list
# breaks the extraction and it would quietly scan against nothing. So: if fewer
# than 60 patterns come back, this exits NON-ZERO rather than reporting clean.
set -u
log="${1:?usage: scan_forbidden.sh <serial-log> [label]}"
label="${2:-scan}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bt="$root/tools/qemu_runner/boot_test.sh"

[ -f "$log" ] || { echo "[$label] FAIL — no capture at $log"; exit 1; }

# Take everything from the assignment to the line closing the printf, then keep
# only single-quoted tokens. Robust to the last entry changing, unlike a sed
# range keyed on a specific final pattern.
# NOTE the `f && NR>s` guard: the assignment line itself carries printf's format
# string, `'%s\n'`, which grep -o would otherwise hand back as a 74th "forbidden
# pattern". Harmless in practice -- no serial log contains a literal %s\n -- but
# a scanner whose pattern count does not match the list it claims to apply is a
# scanner nobody can audit. Measured: 74 patterns before this guard, 73 after,
# against a list of 73.
pats="$(awk '/^GLOBAL_FORBIDDEN=/{f=1; s=NR} f && NR>s {print} f && /\)"$/{exit}' "$bt" \
        | grep -o "'[^']*'" | tr -d "'")"
n="$(printf '%s\n' "$pats" | grep -c .)"
if [ "$n" -lt 60 ]; then
    echo "[$label] FAIL — GLOBAL_FORBIDDEN extraction returned $n patterns (<60)."
    echo "[$label]        The list is empty or the extractor is broken. Refusing"
    echo "[$label]        to report a clean scan against nothing (NON-NEGOTIABLE 6)."
    exit 1
fi

hit=0
while IFS= read -r p; do
    [ -z "$p" ] && continue
    if grep -aqF -- "$p" "$log"; then
        echo "[$label] FAIL — forbidden pattern in capture: $p"
        grep -aF -- "$p" "$log" | head -3
        hit=1
    fi
done <<< "$pats"
[ "$hit" -eq 0 ] || exit 1
echo "[$label] global-forbidden scan clean ($n patterns)"
