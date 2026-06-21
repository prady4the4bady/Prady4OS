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
# Optional patterns (newline-separated) that must NOT appear — e.g. the execve
# "post-exec (BUG)" line that proves execve wrongly returned. Empty by default.
FORBIDDEN_SENTINEL="${FORBIDDEN_SENTINEL:-}"
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
# Optional FAT32 data disk (disk1) for the VFS self-test, and a blank disk
# (disk2) the kernel formats+mounts as SFS — each attached only if present.
FATDISK=()
if [ -f build/fat.img ]; then
    FATDISK=(-drive if=none,format=raw,file=build/fat.img,id=disk1
             -device virtio-blk-pci,drive=disk1)
fi
SFSDISK=()
if [ -f build/sfs.img ]; then
    SFSDISK=(-drive if=none,format=raw,file=build/sfs.img,id=disk2
             -device virtio-blk-pci,drive=disk2)
fi
EXT4DISK=()
if [ -f build/ext4.img ]; then
    EXT4DISK=(-drive if=none,format=raw,file=build/ext4.img,id=disk3
              -device virtio-blk-pci,drive=disk3)
fi

timeout "${TIMEOUT_S}" qemu-system-x86_64 \
    -machine q35 \
    -drive if=none,format=raw,file="$IMG",id=disk0 \
    -device virtio-blk-pci,drive=disk0,bootindex=0 \
    "${FATDISK[@]}" \
    "${SFSDISK[@]}" \
    "${EXT4DISK[@]}" \
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
# Each non-empty line of EXTRA_SENTINEL is a literal pattern that must also appear.
extra_count=0
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    extra_count=$((extra_count + 1))
    if ! grep -qF "$pat" "$SERIAL_LOG"; then
        echo "[smoke] FAIL — required pattern '$pat' not found. Serial output was:"
        cat "$SERIAL_LOG"
        rm -f "$SERIAL_LOG"
        exit 1
    fi
done <<EOF
$EXTRA_SENTINEL
EOF
# Each non-empty line of FORBIDDEN_SENTINEL is a literal pattern that must NOT appear.
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    if grep -qF "$pat" "$SERIAL_LOG"; then
        echo "[smoke] FAIL — forbidden pattern '$pat' appeared. Serial output was:"
        cat "$SERIAL_LOG"
        rm -f "$SERIAL_LOG"
        exit 1
    fi
done <<EOF
$FORBIDDEN_SENTINEL
EOF
echo "[smoke] PASS — saw '$SENTINEL'$( [ "$extra_count" -gt 0 ] && echo " + $extra_count FS pattern(s)" )."
rm -f "$SERIAL_LOG"
exit 0
