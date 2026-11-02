#!/usr/bin/env bash
# docs_only.sh — DDR-1187. Reads the paths a pull request changes, one per
# line, on stdin. Prints `true` when every path is Markdown that no build or
# boot gate reads, else `false`. ci.yml skips the build and the ten boot
# shards only on `true`, so every doubt must answer `false`:
#   - an empty list (the diff produced nothing, or failed quietly) is `false`;
#   - only `*.md` counts: docs/legal/EULA.txt and PRIVACY.txt ride on the ISO
#     and are compared by the ISO gate (Makefile, `cmp ... docs/legal`);
#   - only `docs/**/*.md` and root-level `*.md` count. Root Markdown
#     (CLAUDE.md, SESSION_HANDOFF.md) is still checked by shard-check, which
#     always runs.
# The caller lists renames as delete + add (`git diff --no-renames`), so a
# file moved out of kernel/ into docs/ still shows its kernel/ path.
# Self-test: tools/ci/docs_only_selftest.sh.
set -u
n=0
while IFS= read -r p || [ -n "$p" ]; do
    [ -z "$p" ] && continue
    n=$((n + 1))
    case "$p" in
        docs/*.md) ;;
        */*) echo false; exit 0 ;;
        *.md) ;;
        *) echo false; exit 0 ;;
    esac
done
if [ "$n" -eq 0 ]; then echo false; else echo true; fi
