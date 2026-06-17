#!/usr/bin/env bash
# PRADYOS QEMU smoke test.
# Inert until Phase 1 produces build/pradyos.img. Boots the image headless,
# captures the serial port, and succeeds iff the boot sentinel appears.
set -uo pipefail

IMG="${1:-build/pradyos.img}"
SENTINEL="PRADYOS BOOT OK"
TIMEOUT_S="${TIMEOUT_S:-30}"
SERIAL_LOG="$(mktemp)"

if [ ! -f "$IMG" ]; then
    echo "[smoke] no bootable image at '$IMG' yet — expected during Phase 0."
    echo "[smoke] SKIP (nothing to boot)."
    rm -f "$SERIAL_LOG"
    exit 0
fi

echo "[smoke] booting $IMG (timeout ${TIMEOUT_S}s, sentinel: '$SENTINEL')..."
# -display none + -monitor none keep the guest's COM1 the *only* writer to the
# capture file. Using -nographic instead muxes the QEMU monitor onto the serial
# chardev AND makes SeaBIOS mirror INT 10h console output to COM1, both of which
# corrupt the captured serial stream.
timeout "${TIMEOUT_S}" qemu-system-x86_64 \
    -drive format=raw,file="$IMG" \
    -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" \
    || true

if grep -q "$SENTINEL" "$SERIAL_LOG"; then
    echo "[smoke] PASS — saw '$SENTINEL'."
    rm -f "$SERIAL_LOG"
    exit 0
else
    echo "[smoke] FAIL — sentinel not found. Serial output was:"
    cat "$SERIAL_LOG"
    rm -f "$SERIAL_LOG"
    exit 1
fi
