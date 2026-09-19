#!/usr/bin/env bash
# sym_at.sh <hex-addr> [elf] — name the function containing an address.
# RULE 24 / I24: awk field refs ($1,$3) are EATEN when this is written inline
# through `wsl bash -c`, producing a syntax error or, worse, a silently wrong
# answer. Keep it in a file.
cd "$(dirname "$0")/../.." || exit 2
addr="${1:?usage: sym_at.sh 0xADDR [elf]}"
elf="${2:-build/kernel.elf}"
[ -f "$elf" ] || { echo "sym_at: $elf missing"; exit 2; }
# DDR-1079: this used awk's strtonum(), which is a GAWK EXTENSION. On a host
# whose /usr/bin/awk is mawk -- this development container's is -- every
# invocation died with "function strtonum never defined", so the ONE tool
# §INV.18 and DDR-1019 mandate for resolving a RIP against its own binary failed
# at the exact moment it was needed, while investigating a real CI panic.
# Same class as the mawk defect already fixed once in ci-probe-rodata-check.
# python3 is already a hard dependency of the build (fat-image, mce_inject.py,
# docstate_check.py), so this needs no new tool.
nm -C --defined-only -n "$elf" | python3 -c '
import sys
target = sys.argv[1]
t = int(target, 16)
best = None
for line in sys.stdin:
    parts = line.split(None, 2)
    if len(parts) < 3:
        continue
    try:
        a = int(parts[0], 16)
    except ValueError:
        continue
    if a <= t and (best is None or a >= best[0]):
        best = (a, parts[2].strip())
if best is None:
    print("sym_at: no symbol <= " + target)
    sys.exit(1)
print("sym_at: %s  base=0x%x  target=%s  offset=+0x%x"
      % (best[1], best[0], target, t - best[0]))
' "$addr"
