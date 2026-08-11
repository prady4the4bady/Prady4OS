#!/usr/bin/env bash
# DDR-911: no PRADYOS_* sentinel may be a strict prefix of another.
#
# WHY THIS EXISTS
#
# A gate asserting sentinel A passes when the guest emits sentinel B, if A is a
# prefix of B. That is a check satisfied by something other than what it names —
# the same defect class this project keeps finding, in the naming layer.
#
# It happened for real: a geometry sentinel was added whose name began with the
# existing close sentinel, so the pointer injector matched its own geometry line
# and reported a successful close after one click with nothing closed. (The
# offending name is deliberately not written out here — this file is scanned too,
# and a comment quoting it would trip the very check it documents.)
#
# Substring-anywhere is NOT flagged: sentinels legitimately share stems
# (PRADYOS_FS_*, PRADYOS_SFS_*). Only a strict PREFIX can silently satisfy a
# grep for the shorter name.
#
# Two exclusions, both for non-sentinels the extraction cannot help picking up:
#   * names ending in '_'  — printf format fragments like "PRADYOS_MODE_%s"
#   * names under 12 chars — header guards such as PRADYOS_H
set -u
cd "$(dirname "$0")/../.." || exit 1

names=$(grep -rhoE '\bPRADYOS_[A-Z0-9_]+\b' \
            --include=*.c --include=*.h --include=*.asm \
            --include=Makefile --include=*.sh --include=*.txt \
            kernel user tools Makefile 2>/dev/null \
        | sort -u \
        | grep -vE '_$' \
        | awk 'length($0) >= 12')

[ -n "$names" ] || { echo "sentinel-collision: no sentinels found — check the paths"; exit 1; }

# Pre-existing pairs, each reviewed once and found safe: the only longer form is
# the success variant, and no differing-outcome sibling (a _FAIL) exists that
# could satisfy a gate asserting the shorter name. They are listed explicitly
# rather than renamed, because renaming emitters tree-wide is a large blast
# radius for no behavioural gain — and listing them means any NEW collision
# still fails loudly, which is the point.
ALLOW="
PRADYOS_AMBIANCE|PRADYOS_AMBIANCE_OK
PRADYOS_BACKDROP|PRADYOS_BACKDROP_OK
PRADYOS_BENCH|PRADYOS_BENCH_OK
PRADYOS_DRAG|PRADYOS_DRAG_START
PRADYOS_FOCUS|PRADYOS_FOCUS_KEY
"

count=$(printf '%s\n' "$names" | wc -l)
bad=0
while read -r a; do
    while read -r b; do
        [ "$a" = "$b" ] && continue
        case "$b" in
            "$a"*)
                case "$ALLOW" in
                    *"$a|$b"*) continue ;;      # reviewed, safe (see above)
                esac
                echo "COLLISION: '$a' is a strict prefix of '$b'"
                echo "           a gate asserting '$a' is satisfied by '$b'"
                bad=1
                ;;
        esac
    done <<< "$names"
done <<< "$names"

if [ "$bad" -ne 0 ]; then
    echo "sentinel-collision: FAIL — rename so no sentinel prefixes another,"
    echo "                    or make the asserting gate match a longer form"
    echo "                    (e.g. 'PRADYOS_WM_CLOSE id=')."
    exit 1
fi
echo "sentinel-collision: OK — $count sentinels, no strict-prefix collisions"
