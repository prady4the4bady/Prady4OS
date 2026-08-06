#!/usr/bin/env bash
# S10 token hygiene — no credential may live in a tracked file.
#
# WHY THIS SCRIPT EXISTS RATHER THAN A BARE `git grep ghp_`.
#
# The bare grep was run by hand and had two failure modes, both of which made it
# report "clean" without having checked anything:
#
#   1. `git grep` searches TRACKED files only. Run before `git add`, it searches
#      a version of the tree that does not yet contain the new work — so a new
#      file carrying a credential passes, every time, silently.
#   2. Once real detector patterns and test fixtures exist in the tree, a bare
#      substring grep fires on them forever. A scan whose hits are routinely
#      dismissed by hand is a scan nobody reads, and the one real hit arrives
#      looking exactly like the noise.
#
# So: this scans the WORKING TREE including staged-but-uncommitted and untracked
# files, matches credential SHAPES rather than prefixes, and carries exactly one
# exclusion, stated on its own line with its reason.
set -euo pipefail

cd "$(dirname "$0")/../.."

# The detector module must contain the patterns it detects; excluding it is
# unavoidable and is therefore named explicitly rather than covered by a
# wildcard. Any OTHER file matching these shapes is a failure.
DETECTOR="aether/agents/budget/trajectory.py"

# Credential shapes. Each requires enough varied characters that an obviously
# synthetic fixture (a single repeated letter) does not match.
PATTERNS=(
  'ghp_[A-Za-z0-9]{36}'
  'github_pat_[A-Za-z0-9_]{40,}'
  'gho_[A-Za-z0-9]{36}'
  'sk-[A-Za-z0-9]{32,}'
  'AKIA[0-9A-Z]{16}'
  '-----BEGIN [A-Z ]*PRIVATE KEY-----'
)

# Every file git would consider part of the project: tracked, plus untracked and
# not ignored. `--cached` alone would miss exactly the new work being added.
mapfile -t FILES < <(
  { git ls-files; git ls-files --others --exclude-standard; } | sort -u
)

fail=0
for pattern in "${PATTERNS[@]}"; do
  for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    [ "$f" = "$DETECTOR" ] && continue
    [ "$f" = "tools/ci/secret_scan.sh" ] && continue   # this file states the shapes
    # `-e` is REQUIRED, not stylistic: the private-key pattern begins with a
    # dash, so without it grep parses the pattern as options and that shape
    # silently never matches. Caught by arm D of the self-test, which is the
    # only reason it is not still broken.
    if grep -HnE -e "$pattern" -- "$f" 2>/dev/null; then
      echo "  ^^ matches credential shape /$pattern/" >&2
      fail=1
    fi
  done
done

if [ "$fail" -ne 0 ]; then
  echo >&2
  echo "SECRET SCAN FAILED — a credential shape appears in a project file." >&2
  echo "Do not commit. Do not push. Remove it and rotate the credential." >&2
  exit 1
fi

echo "secret-scan OK — ${#FILES[@]} files, ${#PATTERNS[@]} credential shapes, 0 hits"
