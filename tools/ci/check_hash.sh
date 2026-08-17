#!/usr/bin/env bash
# check_hash.sh — print the kernel image hash. RULE 24.
#
# Why this is a FILE and not an inline command: `$(md5sum build/kernel.bin)`
# passed through `wsl bash -lc "..."` returns an EMPTY string. A Rule 23 check
# written inline therefore compares "" against "" and reports "HASH SAME" — a
# false negative that says "your change was not compiled" when it was. That
# happened while wiring smoke-stack-demand and nearly sent the work down a
# wrong path. Same root cause as the $((p+1)) failure that ab_rate.sh exists
# for: the Windows->WSL inline layer eats shell metacharacters.
#
# Usage:
#   bash tools/ci/check_hash.sh            # print current hash
#   bash tools/ci/check_hash.sh --save     # print and store as the baseline
#   bash tools/ci/check_hash.sh --verify   # compare against the stored baseline
#                                          # exit 0 if CHANGED, 1 if IDENTICAL
# "exit 0 if changed" is deliberate: after a build that should incorporate a
# change, CHANGED is success and IDENTICAL is the failure Rule 23 guards.
cd "$(dirname "$0")/../.." || exit 2
BIN=build/kernel.bin
REF=build/gatelogs/.kernel_hash_baseline

if [ ! -f "$BIN" ]; then
    echo "check_hash: $BIN MISSING — the build did not produce a kernel"
    exit 2
fi
cur=$(md5sum "$BIN" | cut -d' ' -f1)

case "${1:-}" in
    --save)
        mkdir -p "$(dirname "$REF")"
        printf '%s\n' "$cur" > "$REF"
        echo "check_hash: baseline saved $cur"
        ;;
    --verify)
        # NOTE ON SEMANTICS — this bit me once, so it is spelled out.
        # --verify answers "did the binary change since --save?", NOT "is the
        # right kernel built". Those differ: if you --save AFTER building the
        # change, then rebuild the OLD tree, --verify reports CHANGED and that
        # is BAD. Use --expect <hash> when you know which kernel you want.
        if [ ! -f "$REF" ]; then
            echo "check_hash: no baseline stored — run --save first"
            exit 2
        fi
        old=$(cat "$REF")
        echo "check_hash: saved=$old current=$cur"
        if [ "$old" = "$cur" ]; then
            echo "check_hash: IDENTICAL to saved"
            exit 1
        fi
        echo "check_hash: DIFFERENT from saved (this is only 'good' if saved was the PRE-build state)"
        ;;
    --expect)
        # Unambiguous form: name the hash you require. No inference.
        want="${2:?usage: check_hash.sh --expect <md5>}"
        echo "check_hash: want=$want current=$cur"
        if [ "$want" = "$cur" ]; then
            echo "check_hash: MATCH — the intended kernel is built"
        else
            echo "check_hash: MISMATCH — the intended kernel is NOT built (Rule 23)"
            exit 1
        fi
        ;;
    *)
        echo "check_hash: $cur"
        ;;
esac
