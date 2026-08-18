#!/usr/bin/env bash
# sym_at.sh <hex-addr> [elf] — name the function containing an address.
# RULE 24 / I24: awk field refs ($1,$3) are EATEN when this is written inline
# through `wsl bash -c`, producing a syntax error or, worse, a silently wrong
# answer. Keep it in a file.
cd "$(dirname "$0")/../.." || exit 2
addr="${1:?usage: sym_at.sh 0xADDR [elf]}"
elf="${2:-build/kernel.elf}"
[ -f "$elf" ] || { echo "sym_at: $elf missing"; exit 2; }
nm -C --defined-only -n "$elf" | awk -v target="$addr" '
    BEGIN { t = strtonum(target); best = 0; name = "(none)" }
    NF >= 3 {
        a = strtonum("0x" $1)
        if (a <= t && a >= best) { best = a; name = $3 }
    }
    END {
        if (best == 0) { print "sym_at: no symbol <= " target; exit 1 }
        printf "sym_at: %s  base=0x%x  target=%s  offset=+0x%x\n", name, best, target, t - best
    }'
