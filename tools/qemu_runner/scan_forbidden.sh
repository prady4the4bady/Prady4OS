#!/usr/bin/env bash
# [A-3] completion — scan EVERY distinct device configuration any gate uses.
# The first audit enumerated four configurations by hand and missed
# QEMU_SFSROOT, which is exactly how smoke-aether-sfsroot slipped through.
# This derives the list from the Makefile instead of guessing.
set -u
cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS
. "$HOME/.cargo/env"

PATTERNS=$(sed -n "/^GLOBAL_FORBIDDEN=/,/)\"$/p" tools/qemu_runner/boot_test.sh \
           | grep -o "'[^']*'" | tr -d "'" | grep -v '^$')

scan() {
    local label="$1"; shift
    rm -f /tmp/sa.log /tmp/sa_keep.log
    env "$@" SERIAL_LOG=/tmp/sa.log TIMEOUT_S=90 SKIP_GLOBAL_FORBIDDEN=1 \
        EXTRA_SENTINEL='ZZ_NEVER' FORBIDDEN_SENTINEL='ZZ_ALSO_NEVER' \
        bash tools/qemu_runner/boot_test.sh build/pradyos.img >/dev/null 2>&1 &
    local r=$!
    while kill -0 "$r" 2>/dev/null; do
        [ -s /tmp/sa.log ] && cp /tmp/sa.log /tmp/sa_keep.log 2>/dev/null
        sleep 0.3
    done
    wait "$r" || true
    printf '%-34s ' "$label"
    [ -s /tmp/sa_keep.log ] || { echo "NO LOG"; return; }
    local hits=""
    while IFS= read -r p; do
        [ -z "$p" ] && continue
        grep -qaF "$p" /tmp/sa_keep.log && hits="$hits[$p] "
    done <<< "$PATTERNS"
    echo "${hits:-clean}"
}

# Every QEMU_* knob any gate sets, individually and in the combinations gates use.
scan "default"
scan "smp4"                QEMU_SMP=4
scan "gpu"                 QEMU_GPU=1
scan "gpu+smp4"            QEMU_GPU=1 QEMU_SMP=4
scan "nvme"                QEMU_NVME=1
scan "no_ext4+sfs2"        QEMU_NO_EXT4=1 QEMU_SFS2=1
scan "sfsroot"             QEMU_SFSROOT=1
scan "sfsroot+no_ext4"     QEMU_SFSROOT=1 QEMU_NO_EXT4=1
