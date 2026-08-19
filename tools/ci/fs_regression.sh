#!/usr/bin/env bash
# fs_regression.sh — run every FS gate touched by the DDR-954 mount-lifetime fix.
#
# The fix rewrites all ten VFS entry points (each now re-validates the mount
# after taking the lock) and changes vfs_unmount's locking. The blast radius is
# therefore every filesystem gate, not just the one that was failing -- so R10's
# "affected shard" here means the FS gates across shards 0,1,3,4,5.
#
# Verdicts come from the log file, never from an empty grep (R3/T3).
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build/gatelogs
GATES="smoke-sfsroot smoke-aether-sfsroot smoke-fs smoke-fs-rw smoke-fs-sfs-rw smoke-fs-ext4 smoke-mkfs-sfs smoke-sfs-persist smoke-fs-budget"
ref=$(md5sum build/kernel.bin | cut -d' ' -f1)
pass=0; fail=0; failed=""
for g in $GATES; do
    log="build/gatelogs/reg_$g.log"
    make "$g" > "$log" 2>&1
    rc=$?
    if [ ! -s "$log" ]; then
        echo "fs_regression: $g EMPTY LOG — harness failure"; exit 3
    fi
    if grep -aq "^\[smoke\] PASS" "$log" || [ "$rc" -eq 0 ]; then
        pass=$((pass+1)); v=PASS
    else
        fail=$((fail+1)); v=FAIL; failed="$failed $g"
    fi
    u=$(grep -ac "sfs-uaf" "$log")
    echo "fs_regression: $g rc=$rc verdict=$v uaf=$u"
done
echo "fs_regression: ==== PASS=$pass FAIL=$fail ===="
[ -n "$failed" ] && echo "fs_regression: FAILED GATES:$failed"
echo "fs_regression: kernel md5=$ref"
[ "$fail" -eq 0 ]
