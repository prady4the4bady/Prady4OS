#!/usr/bin/env bash
# hygiene_check.sh — run the mandatory pre-push checks and report each rc.
# DDR-1042 added a fourth: ci-resizecheck-selftest, a meta-test over fixtures for
# resize_check.py. The checker decides whether smoke-resizeall passes, and no
# amount of running that gate could have revealed the arm-contamination defect —
# it reported a real-looking failure about a compositor that behaved correctly.
# DDR-1063 added a seventh: ci-docstate-check. It is the FIRST check here that
# reads a claim in a DOCUMENT rather than in the tree -- the other six cover
# shards, probe rodata, _start alignment, the resize checker and apt, and not one
# of them could see that CLAUDE.md's stated kernel.bin headroom was 102,400 B
# wrong for four commits because the size beside it had been updated and the
# subtraction had not.
# RULE 24 (file, not inline): written as an inline `for t in ...; do make $t;
# done` through `wsl bash -c`, $t expands EMPTY. The loop then runs plain `make`
# three times, reports rc=0 three times, and writes every redirect to the same
# file. It looks exactly like three passing checks while running none of them.
# That happened; this file exists so it cannot happen again.
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build/gatelogs
fail=0
for t in ci-shard-check ci-probe-rodata-check ci-start-align-check ci-resizecheck-selftest ci-aptprepare-selftest ci-runnerenv-selftest ci-docstate-check; do
    out="build/gatelogs/${t}.out"
    make "$t" > "$out" 2>&1
    rc=$?
    lines=$(wc -l < "$out")
    echo "hygiene_check: target=$t rc=$rc outlines=$lines log=$out"
    if [ "$rc" -ne 0 ]; then
        fail=1
        echo "hygiene_check: --- $t tail ---"
        tail -20 "$out"
    fi
done
[ "$fail" -eq 0 ] && echo "hygiene_check: ALL SEVEN PASSED"
exit "$fail"
