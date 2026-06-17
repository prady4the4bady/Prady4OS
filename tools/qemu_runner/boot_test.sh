#!/usr/bin/env bash
# PRADYOS QEMU smoke test.
# Inert until Phase 1 produces build/pradyos.img. Boots the image headless,
# captures the serial port, and succeeds iff the boot sentinel appears.
set -uo pipefail

# Default sentinel is the kernel's — it prints only after the full chain
# (Stage 1 -> Stage 2 -> A20/E820/CPUID -> protected mode -> long mode -> ring-0
# C) succeeds, so it subsumes the Phase 1 "PRADYOS BOOT OK" gate. Override with
# SENTINEL=... to test an earlier stage.
IMG="${1:-build/pradyos.img}"
SENTINEL="${SENTINEL:-NEXUS KERNEL OK}"
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
