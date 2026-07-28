#!/usr/bin/env bash
# ADR-034 — boot gate for the aarch64 / riscv64 bootstrap.
#
# Separate from boot_test.sh on purpose: that script hard-codes
# qemu-system-x86_64 and the full virtio/FAT/SFS/ext4/NVMe disk set, none of
# which this boot-only slice has. Threading an arch switch through every disk
# option, for a gate that boots a bare ELF, would complicate the x86_64 harness
# to serve a case it does not share.
#
# Required env: ARCH, QEMU, KERNEL_ELF.  Optional: TIMEOUT_S, SENTINEL.
set -uo pipefail

: "${ARCH:?ARCH is required}"
: "${QEMU:?QEMU is required}"
: "${KERNEL_ELF:?KERNEL_ELF is required}"
TIMEOUT_S="${TIMEOUT_S:-30}"
SENTINEL="${SENTINEL:-NEXUS KERNEL OK}"
SERIAL_LOG="${SERIAL_LOG:-$(mktemp)}"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "[$ARCH] FAIL — $QEMU not installed."
    echo "[$ARCH] (CI installs it; locally: apt-get install qemu-system-arm qemu-system-misc)"
    rm -f "$SERIAL_LOG"
    exit 1
fi
if [ ! -f "$KERNEL_ELF" ]; then
    echo "[$ARCH] FAIL — no kernel at '$KERNEL_ELF'."
    rm -f "$SERIAL_LOG"
    exit 1
fi

case "$ARCH" in
  aarch64)
    # -cpu cortex-a72 per the phase-2 directive. No -bios: `-kernel <ELF>`
    # honours the ELF's own load address and entry point.
    MACHINE=(-machine virt -cpu cortex-a72)
    ;;
  riscv64)
    # -bios default is OpenSBI, which hands off to the payload at 0x80200000
    # in S-mode (ADR-034 platform facts).
    MACHINE=(-machine virt -cpu rv64 -bios default)
    ;;
  *)
    echo "[$ARCH] FAIL — unknown architecture."
    rm -f "$SERIAL_LOG"
    exit 1
    ;;
esac

# -nodefaults: the `virt` machine otherwise instantiates a default device set —
# including a virtio NIC whose option ROM (efi-virtio.rom) is not in Ubuntu's
# qemu-system-arm package, which killed QEMU at startup with an empty serial log
# (run 30380921699: `failed to find romfile "efi-virtio.rom"`).
#
# Installing qemu-system-data would also have silenced it, but this gate boots a
# CPU, some RAM and a UART; instantiating a NIC and a display it never touches is
# the actual mistake. The explicit -serial below still creates the console,
# because -nodefaults suppresses only devices nobody asked for.
echo "[$ARCH] booting $KERNEL_ELF (timeout ${TIMEOUT_S}s, sentinel: '$SENTINEL')..."
timeout "$TIMEOUT_S" "$QEMU" \
    "${MACHINE[@]}" \
    -m 512M -smp 1 \
    -kernel "$KERNEL_ELF" \
    -nodefaults -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" &
qemu_pid=$!

# Same early-exit shape as boot_test.sh (DDR-785): stop as soon as the sentinel
# lands. No forbidden patterns are declared for this gate, so stopping early
# cannot change the verdict.
while kill -0 "$qemu_pid" 2>/dev/null; do
    if grep -q "$SENTINEL" "$SERIAL_LOG" 2>/dev/null; then
        kill "$qemu_pid" 2>/dev/null
        break
    fi
    sleep 0.25
done
wait "$qemu_pid" 2>/dev/null || true

if ! grep -q "$SENTINEL" "$SERIAL_LOG" 2>/dev/null; then
    echo "[$ARCH] FAIL — sentinel '$SENTINEL' not found. Serial output was:"
    cat "$SERIAL_LOG"
    rm -f "$SERIAL_LOG"
    exit 1
fi

echo "[$ARCH] PASS — saw '$SENTINEL'."
sed -n '1,8p' "$SERIAL_LOG"
rm -f "$SERIAL_LOG"
exit 0
