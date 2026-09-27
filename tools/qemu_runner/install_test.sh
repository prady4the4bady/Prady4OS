#!/usr/bin/env bash
# tools/qemu_runner/install_test.sh -- smoke-install, DDR-1143 sec.5 / 10.6 / 10.9.
#
# Every QEMU below runs SEQUENTIALLY: one at a time, each waited on
# (NON-NEGOTIABLE 12).
#
#   boot 1  ISO + a BLANK 128 MiB virtio disk + virtio-rng. PRISM types
#           `install`, then `install 0 WIPE-virtio-blk0`. Requires
#           `[install] ok nonce=N` and captures N.
#   host    the installed disk is read back FROM THE HOST, independently of the
#           kernel that wrote it: stage1 code, PRDI and the table; stage2 +
#           zero pad; the raw kernel == build/kernel.bin + zero gap; the ESP
#           passes fsck.fat and its two files are byte-identical to
#           build/kernel.bin and build/BOOTX64.EFI (mtools); the P2 header.
#           The in-kernel readback proves only that the disk holds what was
#           sent. This proves that what was sent was right.
#   B       boot 2, the DISK ONLY, SeaBIOS: [kimg] src=bios, [root] disk,
#           NOT [ramdisk] formatted SFS, and `[install] mark nonce=N` equal to
#           boot 1's N (arm P, the load-bearing one: only the real P2 can
#           hold a value boot 1's RNG produced).
#   U       boot 2 again, under OVMF: [uefi] handoff plus the same three.
#   neg     a copy of pradyos.img alone (an MBR WITHOUT 'PRDI'): no
#           [root] disk, and no blank-disk widening either.
#
# DDR-1153 (the ledger seed in P2 sector 1) adds five arms:
#   K1 host  P2 sector 1 is PRDYSEED v1; the seed, re-derived by
#            tools/ci/mldsa_ref.py (NOT the kernel's code), gives a pk whose
#            sha256 equals BOTH the stored digest and boot 1's `ledger fp=`.
#   K2 host  ledger_verify.py's fp of the full pk boot 1 printed == that fp.
#   K3       arms B and U print `[ledger] loaded fp=` equal to boot 1's fp.
#   K4       the negative arm prints no `[ledger] loaded`.
#   K5       a copy of the installed disk with ONE seed byte flipped by the
#            host prints `[ledger] seed REFUSED` -- the only arm proving the
#            digest check exists (a kernel that skips it prints `loaded`).
set -uo pipefail
cd "$(dirname "$0")/../.."
D=build/gatelogs/install
mkdir -p "$D"
ISO=build/pradyos.iso
DISK="$D/disk.img"
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC=/usr/share/OVMF/OVMF_VARS_4M.fd
fail() { echo "[install] FAIL: $*"; exit 1; }

# qemu_wait LOG TIMEOUT PATTERN... : run qemu (argv after --) with serial to
# LOG; kill it as soon as every PATTERN has appeared, or at TIMEOUT seconds.
qemu_wait() {
    local log=$1 to=$2; shift 2
    local pats=()
    while [ "$1" != "--" ]; do pats+=("$1"); shift; done
    shift
    : > "$log"
    qemu-system-x86_64 "$@" -no-reboot -display none -monitor none \
        -serial "file:$log" 2>/dev/null &
    local qp=$! t=0
    while kill -0 $qp 2>/dev/null && [ $t -lt $((to * 10)) ]; do
        local all=1
        for p in "${pats[@]}"; do grep -aqF -- "$p" "$log" || { all=0; break; }; done
        [ $all = 1 ] && { sleep 1; break; }
        sleep 0.1; t=$((t + 1))
    done
    kill $qp 2>/dev/null; wait $qp 2>/dev/null
    return 0
}

pgrep -f "[q]emu-system-x86_64" >/dev/null && fail "another QEMU is running (NON-NEGOTIABLE 12)"
[ -f "$ISO" ] || fail "no $ISO"

# ---- boot 1 ----------------------------------------------------------------
rm -f "$DISK" "$D/in.fifo"
truncate -s 128M "$DISK"
L1=$D/boot1.log
: > "$L1"
mkfifo "$D/in.fifo"
timeout 360 qemu-system-x86_64 -machine q35 -cdrom "$ISO" -boot d \
    -drive "file=$DISK,format=raw,if=none,id=d0" -device virtio-blk-pci,drive=d0 \
    -device virtio-rng-pci \
    -no-reboot -display none -monitor none -serial stdio < "$D/in.fifo" > "$L1" 2>/dev/null &
QP=$!
# The feeder holds the fifo's write end for the whole boot and, once PRISM
# has answered, stops QEMU itself: PRISM's `exit` does not power the guest
# off, so waiting for QEMU would idle out the remaining ~5 min of timeout.
( exec > "$D/in.fifo"
  for i in $(seq 1 1800); do grep -aq PRISM_READY "$L1" && break; sleep 0.1; done
  sleep 2; printf 'install\n'
  sleep 3; printf 'install 0 WIPE-virtio-blk0\n'
  for i in $(seq 1 2400); do grep -aqE 'install: (done|refused)' "$L1" && break; sleep 0.1; done
  sleep 1; kill $QP 2>/dev/null ) &
FEED=$!
wait $QP 2>/dev/null
wait $FEED 2>/dev/null
rm -f "$D/in.fifo"
grep -aq '\[ramdisk\] blank disk blk0 kept' "$L1" || fail "boot 1: blank-disk widening did not fire"
grep -aqE 'install: disk 0 virtio-blk 128 MiB phys blank' "$L1" || fail "boot 1: disk list did not report a blank phys disk"
N=$(grep -aoE '\[install\] ok nonce=[0-9a-f]{16}' "$L1" | head -1 | sed 's/.*=//')
[ -n "$N" ] || { tail -20 "$L1"; fail "boot 1: no [install] ok nonce="; }
echo "[install] boot 1 ok nonce=$N"
FP=$(grep -aoE '\[install\] ledger fp=[0-9a-f]{16}' "$L1" | head -1 | sed 's/.*=//')
[ -n "$FP" ] || fail "boot 1: no [install] ledger fp="
K2=$(python3 tools/ci/ledger_verify.py "$L1" fp) || fail "arm K2: ledger_verify could not read the printed pk"
[ "$K2" = "$FP" ] || fail "arm K2: printed pk fp=$K2 is not the printed ledger fp=$FP"
echo "[install] arm K2 ok (printed pk fp=$FP)"

# ---- host readback ---------------------------------------------------------
python3 - "$DISK" <<'PY' || fail "host readback"
import sys, struct
d = open(sys.argv[1], 'rb').read()
k = open('build/kernel.bin', 'rb').read()
s1 = open('build/stage1.bin', 'rb').read()
s2 = open('build/stage2.bin', 'rb').read()
m = d[:512]
ok = True
def chk(name, c):
    global ok
    print('[install] host', name, 'ok' if c else 'FAIL'); ok &= c
chk('stage1', m[:440] == s1[:440] and m[510:] == b'\x55\xaa')
chk('sig', m[440:444] == b'PRDI')
e0 = struct.unpack('<BxxxBxxxII', m[446:462]); e1 = struct.unpack('<BxxxBxxxII', m[462:478])
chk('p1', e0[1] == 0xEF and e0[2] == 4096 and e0[3] == 131072)
chk('p2', e1[1] == 0xDA and e1[2] == 135168 and e1[2] + e1[3] == len(d) // 512)
chk('stage2', d[512:512 + len(s2)] == s2 and not any(d[512 + len(s2):17 * 512]))
chk('kernel', d[17 * 512:17 * 512 + len(k)] == k and not any(d[17 * 512 + len(k):4096 * 512]))
h = d[135168 * 512:135168 * 512 + 20]
chk('p2hdr', h[:8] == b'PRDYVOL1' and struct.unpack('<III', h[8:20]) == (1, 1, 8))
open('build/gatelogs/install/esp.img', 'wb').write(d[4096 * 512:(4096 + 131072) * 512])
sys.exit(0 if ok else 1)
PY

# ---- K1: the persisted seed, re-derived OFF the machine ----------------------
python3 - "$DISK" "$FP" <<'PY' || fail "arm K1"
import sys, struct, hashlib
sys.path.insert(0, 'tools/ci')
from mldsa_ref import keygen
d = open(sys.argv[1], 'rb').read()
sec = d[(135168 + 1) * 512:(135168 + 2) * 512]
if sec[:8] != b'PRDYSEED' or struct.unpack('<I', sec[8:12])[0] != 1:
    sys.exit('[install] arm K1 FAIL: P2 sector 1 is not PRDYSEED v1')
seed, dig = sec[12:44], sec[44:76]
if any(sec[76:]):
    sys.exit('[install] arm K1 FAIL: sector 1 tail is not zero')
pk, _ = keygen(seed)
h = hashlib.sha256(pk).digest()
if h != dig:
    sys.exit('[install] arm K1 FAIL: keygen(seed) does not match the stored digest')
if h.hex()[:16] != sys.argv[2]:
    sys.exit(f'[install] arm K1 FAIL: persisted key {h.hex()[:16]} is not the printed fp {sys.argv[2]}')
print(f'[install] arm K1 ok (seed -> pk fp={h.hex()[:16]}, independent keygen)')
PY
fsck.fat -n "$D/esp.img" >/dev/null 2>&1 || fail "host: fsck.fat rejects the ESP"
rm -f "$D/k.out" "$D/e.out"
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$D/esp.img" ::/KERNEL.BIN "$D/k.out" || fail "host: no ::/KERNEL.BIN"
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$D/esp.img" ::/EFI/BOOT/BOOTX64.EFI "$D/e.out" || fail "host: no ::/EFI/BOOT/BOOTX64.EFI"
cmp -s "$D/k.out" build/kernel.bin || fail "host: ESP KERNEL.BIN differs from build/kernel.bin"
cmp -s "$D/e.out" build/BOOTX64.EFI || fail "host: ESP BOOTX64.EFI differs from build/BOOTX64.EFI"
echo "[install] host esp fsck=ok files=identical"

# ---- boot 2, BIOS ----------------------------------------------------------
LB=$D/boot2_bios.log
qemu_wait "$LB" 180 "[install] mark nonce=" "PRISM_READY" -- \
    -machine q35 -drive "file=$DISK,format=raw,if=none,id=d0" \
    -device virtio-blk-pci,drive=d0,bootindex=0 -device virtio-rng-pci
grep -aqF '[kimg] src=bios' "$LB" || fail "arm B: no [kimg] src=bios"
grep -aqF '[root] disk p2 lba=135168' "$LB" || fail "arm B: no [root] disk"
grep -aqF '[ramdisk] formatted SFS' "$LB" && fail "arm B: took the live ramdisk root"
grep -aqF "[install] mark nonce=$N" "$LB" || { grep -a 'mark' "$LB"; fail "arm B/P: mark is not boot 1's nonce $N"; }
grep -aqF 'PRISM_READY' "$LB" || fail "arm B: PRISM never started from the installed root"
grep -aqF "[ledger] loaded fp=$FP" "$LB" || { grep -a '\[ledger\]' "$LB"; fail "arm B/K3: did not load the installed ledger key $FP"; }
echo "[install] arm B ok (bios, root=p2, mark=$N, ledger fp=$FP)"

# ---- boot 2, UEFI ----------------------------------------------------------
[ -f "$OVMF_CODE" ] && [ -f "$OVMF_VARS_SRC" ] || fail "OVMF not installed"
cp "$OVMF_VARS_SRC" "$D/vars.fd"
LU=$D/boot2_uefi.log
qemu_wait "$LU" 240 "[install] mark nonce=" "PRISM_READY" -- \
    -machine q35 \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,unit=1,file=$D/vars.fd" \
    -drive "file=$DISK,format=raw,if=none,id=d0" \
    -device virtio-blk-pci,drive=d0,bootindex=0 -device virtio-rng-pci
grep -aqF '[uefi] handoff' "$LU" || { tail -20 "$LU"; fail "arm U: no [uefi] handoff"; }
grep -aqF '[kimg] src=uefi' "$LU" || fail "arm U: no [kimg] src=uefi"
grep -aqF '[root] disk p2 lba=135168' "$LU" || fail "arm U: no [root] disk"
grep -aqF '[ramdisk] formatted SFS' "$LU" && fail "arm U: took the live ramdisk root"
grep -aqF "[install] mark nonce=$N" "$LU" || fail "arm U/P: mark is not boot 1's nonce $N"
grep -aqF "[ledger] loaded fp=$FP" "$LU" || fail "arm U/K3: did not load the installed ledger key $FP"
echo "[install] arm U ok (uefi, root=p2, mark=$N, ledger fp=$FP)"

# ---- negative --------------------------------------------------------------
cp build/pradyos.img "$D/neg.img"          # a COPY: blk_test_thread writes LBA 4095
LN=$D/neg.log
qemu_wait "$LN" 120 "[fs] no mountable filesystem found" -- \
    -machine q35 -drive "file=$D/neg.img,format=raw,if=none,id=d0" \
    -device virtio-blk-pci,drive=d0,bootindex=0 -device virtio-rng-pci
grep -aqF 'NEXUS KERNEL OK' "$LN" || fail "neg: kernel did not boot"
grep -aqF '[root] disk' "$LN" && fail "neg: a disk without PRDI was selected as root"
grep -aqF 'blank disk blk0' "$LN" && fail "neg: a non-blank disk took the blank-disk widening"
grep -aqF '[ledger] loaded' "$LN" && fail "neg/K4: loaded a ledger key from a disk that is not installed"
echo "[install] neg ok (foreign MBR: no root selection, no ledger key)"

# ---- K5: a flipped seed byte must be REFUSED ----------------------------------
cp "$DISK" "$D/bad.img"
python3 - "$D/bad.img" <<'PY' || fail "arm K5: could not corrupt the copy"
import sys
f = open(sys.argv[1], 'r+b')
f.seek((135168 + 1) * 512 + 12 + 5)
b = f.read(1); f.seek(-1, 1); f.write(bytes([b[0] ^ 0x01])); f.close()
PY
LK=$D/bad.log
qemu_wait "$LK" 180 "[root] disk p2" -- \
    -machine q35 -drive "file=$D/bad.img,format=raw,if=none,id=d0" \
    -device virtio-blk-pci,drive=d0,bootindex=0 -device virtio-rng-pci
grep -aqF '[ledger] seed REFUSED' "$LK" || { grep -a '\[ledger\]\|\[root\]' "$LK"; fail "arm K5: a corrupted seed was not refused"; }
grep -aqF '[ledger] loaded' "$LK" && fail "arm K5: a corrupted seed was loaded"
echo "[install] arm K5 ok (flipped seed byte -> REFUSED)"
rm -f "$D/bad.img"
echo "[install] PASS nonce=$N ledger fp=$FP"
