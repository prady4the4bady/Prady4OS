#!/usr/bin/env bash
# docsize_check.sh — #45 comment 6229808666 item 4.
# CLAUDE.md is loaded into every session, and SESSION_HANDOFF.md is read at
# every session start. Their size is a recurring token cost, so both are capped
# at 40 KB. Older content goes to docs/archive/; nothing is deleted.
# Fixture: run with DOCSIZE_LIMIT=1 to see it fail.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LIMIT="${DOCSIZE_LIMIT:-40960}"
fail=0
for f in CLAUDE.md SESSION_HANDOFF.md; do
    if [ ! -f "$ROOT/$f" ]; then echo "docsize: FAIL - $f missing"; fail=1; continue; fi
    n=$(wc -c < "$ROOT/$f")
    if [ "$n" -gt "$LIMIT" ]; then
        echo "docsize: FAIL - $f is $n B > $LIMIT B; move history to docs/archive/"
        fail=1
    else
        echo "docsize: $f $n B <= $LIMIT B"
    fi
done
exit $fail
