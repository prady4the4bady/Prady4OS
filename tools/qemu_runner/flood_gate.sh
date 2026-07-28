#!/usr/bin/env bash
# DDR-797 — serial-flood regression gate.
#
# Boots normally and asserts the serial capture stays under a byte ceiling.
# Separate from boot_test.sh because that script asserts on CONTENT (sentinels
# present/absent) and this asserts on VOLUME — a boot can contain every required
# sentinel and still be drowning in garbage, which is exactly the state DDR-791
# documented and DDR-797 fixed.
#
# Measured: ~11.5 KiB after the fix, 97.5 KiB before it. The 32 KiB ceiling
# therefore discriminates by ~3x in each direction rather than sitting at a
# hair's breadth, so ordinary boot-to-boot variation cannot flip it.
set -uo pipefail

IMG="${1:-build/pradyos.img}"
TIMEOUT_S="${TIMEOUT_S:-60}"
MAX_BYTES="${MAX_BYTES:-32768}"
SENTINEL="${SENTINEL:-NEXUS KERNEL OK}"
SERIAL_LOG="${SERIAL_LOG:-$(mktemp)}"

if [ ! -f "$IMG" ]; then
    echo "[flood] FAIL — no image at '$IMG'."
    rm -f "$SERIAL_LOG"; exit 1
fi

echo "[flood] booting $IMG (timeout ${TIMEOUT_S}s, ceiling ${MAX_BYTES} bytes)..."
timeout "$TIMEOUT_S" qemu-system-x86_64 \
    -machine q35 \
    -drive if=none,format=raw,file="$IMG",id=disk0 \
    -device virtio-blk-pci,drive=disk0,bootindex=0 \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -smp 4 -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" &
qemu_pid=$!
wait "$qemu_pid" 2>/dev/null || true

if [ ! -s "$SERIAL_LOG" ]; then
    echo "[flood] FAIL — no serial output at all."
    rm -f "$SERIAL_LOG"; exit 1
fi

# The boot must be REAL. Without this a kernel that died instantly would emit
# almost nothing and sail under the ceiling — the gate would reward a worse
# failure than the one it exists to catch.
if ! grep -q "$SENTINEL" "$SERIAL_LOG"; then
    echo "[flood] FAIL — '$SENTINEL' absent; the boot did not get far enough to measure."
    rm -f "$SERIAL_LOG"; exit 1
fi

total=$(wc -c < "$SERIAL_LOG")
binary=$(tr -d '[:print:][:space:]' < "$SERIAL_LOG" | wc -c)
pct=$(( binary * 100 / total ))

echo "[flood] serial bytes: $total (non-printable $binary, ${pct}%)"
if [ "$total" -gt "$MAX_BYTES" ]; then
    echo "[flood] FAIL — $total bytes exceeds the ${MAX_BYTES}-byte ceiling."
    echo "[flood] (DDR-797: a boot this loud is a probe dumping memory to the console.)"
    rm -f "$SERIAL_LOG"; exit 1
fi
echo "[flood] PASS — $total bytes, under the ${MAX_BYTES}-byte ceiling."
rm -f "$SERIAL_LOG"
exit 0
