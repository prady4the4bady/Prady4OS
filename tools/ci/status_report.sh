#!/usr/bin/env bash
# tools/ci/status_report.sh — generated progress numbers (directive §6.5).
#
# WHY: every session hand-tallies progress into SESSION_HANDOFF.md, which is
# both slow and how "gate count: 105" survived in CLAUDE.md long after the real
# number passed 145. Numbers that are derived should be derived.
#
# Counts open vs done items per Group from docs/BUILD_TRACKER.md and
# CLAUDE.md's backlog tables. A row is DONE when it is struck through (~~...~~),
# ticked ([x]), or marked COMPLETE/DONE/CLOSED/FIXED; otherwise it is OPEN.
# That heuristic is stated here rather than hidden, because it is the thing to
# check first if a number ever looks wrong.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

printf '## Generated status — %s\n\n' "$(date -u +%FT%TZ)"

printf '### Gates\n'
tot=$(awk -F'\t' '/^[0-9]/ && NF>=3' tools/ci/gate_shards.txt | wc -l)
sh=$(awk -F'\t' '/^[0-9]/{print $1}' tools/ci/gate_shards.txt | sort -u | wc -l)
fast=$(awk -F'\t' '$4=="fast"' tools/ci/gate_shards.txt | wc -l)
mk=$(awk -F'\t' '/^[0-9]/ && NF>=3 {t[$1]+=$3} END {m=0; for (s in t) if (t[s]>m) m=t[s]; printf "%.1f", m/60}' tools/ci/gate_shards.txt)
printf -- '- %s gates across %s shards; makespan %s min; %s fast-tier / %s strict-tier\n\n' \
    "$tot" "$sh" "$mk" "$fast" "$((tot-fast))"

printf '### Backlog by Group (CLAUDE.md tables)\n\n'
printf '| Group | done | open | total |\n|---|---:|---:|---:|\n'
# Portable single pass. The union of groups is tracked explicitly in seen[] —
# an earlier version iterated `for (k in d)` and silently DROPPED every group
# with zero done rows (C, E, G and H all vanished), which is the same class of
# undercount this script exists to prevent.
awk '
  /^### GROUP [A-H]/ { g=$3; seen[g]=1; next }
  /^## [^#]/ { g="" }
  g && /^\| / {
    if ($0 ~ /^\|[-: |]+\|$/) next
    if ($0 ~ /^\| *Item *\|/) next
    if ($0 ~ /~~/ || $0 ~ /\[x\]/ || $0 ~ /COMPLETE|DONE|CLOSED|FIXED|✅/) d[g]++
    else o[g]++
  }
  END {
    split("A B C D E F G H", ord, " ")
    for (i=1; i<=8; i++) {
      k=ord[i]
      if (!(k in seen)) continue
      printf "| %s | %d | %d | %d |\n", k, d[k]+0, o[k]+0, d[k]+o[k]
      td+=d[k]+0; to+=o[k]+0
    }
    printf "| **all** | **%d** | **%d** | **%d** |\n", td, to, td+to
  }
' CLAUDE.md

printf '\n### Open issues\n'
for i in OPEN-1 OPEN-12 OPEN-13; do
  if grep -q "^| \*\*$i\*\*" CLAUDE.md 2>/dev/null; then printf -- '- %s: OPEN\n' "$i"; fi
done
for i in OPEN-2 OPEN-10 OPEN-11 FSRM; do
  if grep -q "~~$i~~\|~~\*\*$i" CLAUDE.md 2>/dev/null; then printf -- '- %s: closed\n' "$i"; fi
done

printf '\n### Kernel\n'
if [ -f build/kernel.bin ]; then
  sz=$(stat -c%s build/kernel.bin)
  printf -- '- kernel.bin %s B / 1572864 B gate (%s B headroom)\n' "$sz" "$((1572864-sz))"
  printf -- '- sha256 %s\n' "$(sha256sum build/kernel.bin | cut -d" " -f1)"
else
  printf -- '- kernel.bin not built\n'
fi
printf -- '- HEAD %s on %s\n' "$(git rev-parse --short HEAD)" "$(git rev-parse --abbrev-ref HEAD)"
