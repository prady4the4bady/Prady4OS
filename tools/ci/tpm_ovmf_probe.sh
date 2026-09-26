#!/usr/bin/env bash
# DDR-1145 §1.1 — does the distribution OVMF talk to a TPM at all?
#
# Boots Ubuntu's OVMF_CODE_4M.fd (no disk, no OS) against swtpm on BOTH TPM
# interfaces QEMU models (tpm-crb, tpm-tis), logs every command the firmware
# sends, and decodes the command codes. The claim it checks is narrow:
#
#   PASS  the firmware extends PCR 4 (a DXE-phase measurement, which is where
#         Tcg2Dxe runs and installs EFI_TCG2_PROTOCOL) on BOTH interfaces, and
#         extends PCR 9 on NEITHER (PCR 9 is the one DDR-1145 §3 measures the
#         kernel into, so firmware must leave it alone).
#   FAIL  otherwise, with the decoded command histogram printed.
#
# NOT claimed: that our loader can LocateProtocol(EFI_TCG2_PROTOCOL). That is an
# inference from Tcg2Dxe having run, and is measured directly by the loader's
# own first call when DDR-1145 is built.
#
# Needs: qemu-system-x86_64, ovmf, swtpm (Ubuntu archive). Logs under
# build/gatelogs/tpmprobe (NON-NEGOTIABLE 7). One QEMU at a time (NN 12).
set -uo pipefail
cd "$(dirname "$0")/../.."
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
W=build/gatelogs/tpmprobe
for t in qemu-system-x86_64 swtpm python3; do
    command -v "$t" >/dev/null || { echo "[tpmprobe] FAIL -- missing $t"; exit 2; }
done
[ -f "$OVMF_CODE" ] || { echo "[tpmprobe] FAIL -- no $OVMF_CODE"; exit 2; }
if pgrep -f "[q]emu-system-x86_64" >/dev/null; then
    echo "[tpmprobe] FAIL -- another QEMU is running (NON-NEGOTIABLE 12)"; exit 2
fi
rm -rf "$W"; mkdir -p "$W"
W=$(cd "$W" && pwd)
rc=0
for dev in tpm-crb tpm-tis; do
    st="$W/state-$dev"; mkdir -p "$st"
    cp "$OVMF_VARS" "$W/vars-$dev.fd"
    swtpm socket --tpm2 --tpmstate dir="$st" \
        --ctrl type=unixio,path="$st/sock" \
        --log file="$W/swtpm-$dev.log",level=20 \
        --flags not-need-init,startup-clear >/dev/null 2>&1 &
    sp=$!
    for _ in $(seq 50); do [ -S "$st/sock" ] && break; sleep 0.1; done
    timeout 40 qemu-system-x86_64 -machine q35 -m 256 -nographic -no-reboot \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$W/vars-$dev.fd" \
        -chardev socket,id=chrtpm,path="$st/sock" \
        -tpmdev emulator,id=tpm0,chardev=chrtpm -device "$dev",tpmdev=tpm0 \
        -serial file:"$W/serial-$dev.log" -monitor none -display none \
        >"$W/qemu-$dev.out" 2>&1
    kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
    python3 - "$W/swtpm-$dev.log" "$dev" <<'EOF' || rc=1
import sys, re, collections
path, dev = sys.argv[1], sys.argv[2]
NAMES = {0x144: 'Startup', 0x143: 'SelfTest', 0x17A: 'GetCapability',
         0x182: 'PCR_Extend', 0x17E: 'PCR_Read', 0x186: 'HashSequenceStart'}
lines = open(path, errors='replace').read().split('\n')
cmds, i = [], 0
while i < len(lines):
    if 'SWTPM_IO_Read' in lines[i]:
        hx, j = [], i + 1
        while j < len(lines) and re.fullmatch(r'\s*([0-9A-F]{2}\s*)+', lines[j]):
            hx += lines[j].split(); j += 1
        b = bytes(int(h, 16) for h in hx)
        if len(b) >= 10:
            cmds.append((int.from_bytes(b[6:10], 'big'), b))
        i = j
    else:
        i += 1
hist = collections.Counter(NAMES.get(c, hex(c)) for c, _ in cmds)
pcrs = collections.Counter(int.from_bytes(b[10:14], 'big') for c, b in cmds if c == 0x182)
print(f"[tpmprobe] {dev}: {len(cmds)} commands {dict(hist)}")
print(f"[tpmprobe] {dev}: PCR_Extend by PCR {dict(sorted(pcrs.items()))}")
if pcrs.get(4, 0) == 0:
    print(f"[tpmprobe] FAIL -- {dev}: firmware never extended PCR 4 (no DXE TCG2 measurement)")
    sys.exit(1)
if pcrs.get(9, 0) != 0:
    print(f"[tpmprobe] FAIL -- {dev}: firmware extended PCR 9, which DDR-1145 reserves for the kernel")
    sys.exit(1)
EOF
done
if [ $rc -eq 0 ]; then
    echo "[tpmprobe] PASS -- OVMF measures into the TPM on tpm-crb and tpm-tis; PCR 9 left untouched"
fi
exit $rc
