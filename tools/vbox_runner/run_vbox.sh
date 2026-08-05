#!/usr/bin/env bash
# tools/vbox_runner/run_vbox.sh — boot build/pradyos.img under VirtualBox and
# assert the boot sentinel (DDR-841, Group 1 item 4; Delivery D.2).
#
# Same contract as tools/qemu_runner/boot_test.sh, different hypervisor: boot,
# capture serial, require a sentinel, forbid a pattern.
#
# EXIT CODES — the 77 matters:
#   0   sentinel found
#   1   booted, sentinel absent (a real failure)
#   77  VirtualBox is NOT AVAILABLE on this machine
#
# 77 is deliberately NOT 0. CI runners have no VirtualBox and nested VirtualBox
# is unsupported, so this script cannot run there. A runner that returned 0 when
# the hypervisor is missing would let a green pipeline imply a VirtualBox boot
# that never happened — the recurring "reports success without checking" defect
# this project has now hit four times in the build system alone. The caller must
# decide what "not available" means; this script refuses to decide for it.
set -u

IMG="${1:-build/pradyos.img}"
SENTINEL="${SENTINEL:-NEXUS KERNEL OK}"
FORBIDDEN="${FORBIDDEN:-}"
TIMEOUT_S="${TIMEOUT_S:-90}"
VM="${VM:-pradyos-d2}"

if ! command -v VBoxManage >/dev/null 2>&1; then
    echo "[vbox] VBoxManage not found — VirtualBox is not available here."
    echo "[vbox] This is NOT a pass. Run this on a machine with VirtualBox for D.2."
    exit 77
fi

if [ ! -f "$IMG" ]; then
    echo "[vbox] FAIL — image not found: $IMG (run 'make image' first)"
    exit 1
fi

WORK="$(mktemp -d)"
VDI="$WORK/pradyos.vdi"
SERIAL="$WORK/serial.log"

cleanup() {
    VBoxManage controlvm "$VM" poweroff >/dev/null 2>&1 || true
    VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "[vbox] converting $IMG -> VDI"
VBoxManage convertfromraw "$IMG" "$VDI" --format VDI >/dev/null || {
    echo "[vbox] FAIL — convertfromraw"; exit 1; }

# A previous run that died hard can leave the VM registered; remove it first so
# this script is re-runnable rather than failing on the second invocation.
VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true

VBoxManage createvm --name "$VM" --ostype Other_64 --register >/dev/null || {
    echo "[vbox] FAIL — createvm"; exit 1; }
VBoxManage modifyvm "$VM" --memory 512 --firmware bios --boot1 disk \
    --uart1 0x3F8 4 --uartmode1 file "$SERIAL" >/dev/null
VBoxManage storagectl "$VM" --name SATA --add sata --controller IntelAhci >/dev/null
VBoxManage storageattach "$VM" --storagectl SATA --port 0 --device 0 \
    --type hdd --medium "$VDI" >/dev/null

echo "[vbox] booting headless (timeout ${TIMEOUT_S}s, sentinel: '$SENTINEL')"
VBoxManage startvm "$VM" --type headless >/dev/null || {
    echo "[vbox] FAIL — startvm"; exit 1; }

found=1
for _ in $(seq 1 "$TIMEOUT_S"); do
    if [ -f "$SERIAL" ] && grep -qF "$SENTINEL" "$SERIAL" 2>/dev/null; then
        found=0
        break
    fi
    sleep 1
done

VBoxManage controlvm "$VM" poweroff >/dev/null 2>&1 || true

if [ -n "$FORBIDDEN" ] && [ -f "$SERIAL" ] && grep -qF "$FORBIDDEN" "$SERIAL"; then
    echo "[vbox] FAIL — forbidden pattern '$FORBIDDEN' appeared. Serial:"
    cat "$SERIAL"
    exit 1
fi

if [ "$found" -ne 0 ]; then
    echo "[vbox] FAIL — sentinel '$SENTINEL' not found. Serial output was:"
    [ -f "$SERIAL" ] && cat "$SERIAL" || echo "(no serial captured)"
    exit 1
fi

echo "[vbox] PASS — saw '$SENTINEL' under VirtualBox"
exit 0
