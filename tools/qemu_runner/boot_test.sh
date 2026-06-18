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
# Optional second pattern that must ALSO appear (e.g. the FAT32 self-test line).
# Empty by default so the plain kernel gate only checks the boot sentinel.
EXTRA_SENTINEL="${EXTRA_SENTINEL:-}"
TIMEOUT_S="${TIMEOUT_S:-30}"
SERIAL_LOG="$(mktemp)"

if [ ! -f "$IMG" ]; then
    echo "[smoke] no bootable image at '$IMG' yet — expected during Phase 0."
    echo "[smoke] SKIP (nothing to boot)."
    rm -f "$SERIAL_LOG"
    exit 0
fi

echo "[smoke] booting $IMG (timeout ${TIMEOUT_S}s, sentinel: '$SENTINEL')..."
# q35 gives a PCIe machine with an ACPI MCFG table (MMCONFIG/ECAM) for Phase 3
# enumeration. Boot from a virtio-blk disk and add a virtio-net device so the
# PCIe scan has real devices to find. -display none + -monitor none keep the
# guest's COM1 the only writer to the capture file (see git history for why
# -nographic corrupts it).
# Optional FAT32 data disk for the VFS self-test (second virtio-blk).
FATDISK=()
if [ -f build/fat.img ]; then
    FATDISK=(-drive if=none,format=raw,file=build/fat.img,id=disk1
             -device virtio-blk-pci,drive=disk1)
fi

timeout "${TIMEOUT_S}" qemu-system-x86_64 \
    -machine q35 \
    -drive if=none,format=raw,file="$IMG",id=disk0 \
    -device virtio-blk-pci,drive=disk0,bootindex=0 \
    "${FATDISK[@]}" \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" \
    || true

if ! grep -q "$SENTINEL" "$SERIAL_LOG"; then
    echo "[smoke] FAIL — kernel sentinel '$SENTINEL' not found. Serial output was:"
    cat "$SERIAL_LOG"
    rm -f "$SERIAL_LOG"
    exit 1
fi
if [ -n "$EXTRA_SENTINEL" ] && ! grep -qF "$EXTRA_SENTINEL" "$SERIAL_LOG"; then
    echo "[smoke] FAIL — extra sentinel '$EXTRA_SENTINEL' not found. Serial output was:"
    cat "$SERIAL_LOG"
    rm -f "$SERIAL_LOG"
    exit 1
fi
echo "[smoke] PASS — saw '$SENTINEL'${EXTRA_SENTINEL:+ and '$EXTRA_SENTINEL'}."
rm -f "$SERIAL_LOG"
exit 0
