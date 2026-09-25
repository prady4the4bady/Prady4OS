#!/usr/bin/env bash
# tools/ci/gop_gate.sh -- smoke-gop (DDR-1142): the UEFI GOP framebuffer path.
#
# PROXY, STATED: no physical UEFI machine is available here. The closest
# verifiable proxy is OVMF on QEMU's std-vga (q35's default VGA): OVMF's
# QemuVideoDxe publishes a real Graphics Output Protocol over a real PCI BAR,
# so HandleProtocol -> FrameBufferBase -> pixels in the scanout is the same
# path a physical board runs. What is NOT covered is a vendor's GOP driver.
#
# Arms:
#   G  the handoff arrived and the kernel chose it: an exact "[fb] gop" line,
#      values pinned from the proxy's first measured boot.
#   S  the pixels reached the SCANOUT: a QMP screendump -- what the emulated
#      display is showing, not what the CPU wrote -- must show the kernel's
#      four quadrant colours at each quadrant centre. "Write a pattern and read
#      it back" is VACUOUS (plain RAM at a wrong address reads back fine); the
#      screendump is the one observation a wrong base/stride cannot fake.
#   C  the ring-3 compositor drew on it: after PRADYOS_AMBIANCE_OK a second
#      screendump must show all four quadrant centres CHANGED from the kernel
#      pattern. This is the only arm that covers sys_fb_map's physical-address
#      mapping; a wrong user mapping leaves the kernel's pattern on the screen.
#   B  the BIOS path says "[fb] gop none" (INV.13: stated, not inherited).
#
# No virtio-gpu is attached, so GOP is the only display the kernel can use.
set -u
ESP="${1:-build/esp.img}"
IMG="${2:-build/pradyos.img}"
OUT=build/gatelogs
mkdir -p "$OUT"
LOG="$OUT/gop.log"; DUMP="$OUT/gop.ppm"; DUMP2="$OUT/gop_desktop.ppm"; VARS="$OUT/gop_vars.fd"
SOCK="$OUT/gop.qmp"
EXPECT_G='[fb] gop 1280x800 stride=1280 fmt=1 base=0x0000000080000000 used=1'
TIMEOUT_S="${TIMEOUT_S:-150}"

if pgrep -f "[q]emu-system-x86_64" >/dev/null; then
    echo "[gop] FAIL: another QEMU is running (NON-NEGOTIABLE 12)"; exit 1
fi
# Assert the disk images exist rather than let QEMU fail to open them (DDR-1130's
# rule: a precondition is checked, never left to surface as a different
# failure). CI's shared build artefact deliberately excludes *.img (DDR-1035),
# so the make target must build them; this names it if it did not.
for f in "$ESP" "$IMG" build/fat.img build/sfs.img /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_VARS_4M.fd; do
    [ -f "$f" ] || { echo "[gop] FAIL: precondition -- $f is missing (make smoke-gop builds the images)"; exit 1; }
done
rm -f "$LOG" "$DUMP" "$DUMP2" "$SOCK"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$VARS"

timeout "$TIMEOUT_S" qemu-system-x86_64 -M q35 -m 512M \
    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive "if=pflash,format=raw,unit=1,file=$VARS" \
    -drive "if=none,format=raw,file=$ESP,id=esp" -device virtio-blk-pci,drive=esp,bootindex=0 \
    -drive if=none,format=raw,file=build/fat.img,id=d1 -device virtio-blk-pci,drive=d1 \
    -drive if=none,format=raw,file=build/sfs.img,id=d2 -device virtio-blk-pci,drive=d2 \
    -fw_cfg name=opt/org.pradyos/probes,string=gop \
    -qmp "unix:$SOCK,server,nowait" \
    -no-reboot -display none -monitor none -serial "file:$LOG" 2>"$OUT/gop.qemu.err" &
qpid=$!

got=0
for _ in $(seq 1 $((TIMEOUT_S * 10))); do
    if grep -aq '\[fb\] gop pattern drawn\|\[fb\] gop none\|\[fb\] gop rejected\|used=0' "$LOG" 2>/dev/null; then
        got=1; break
    fi
    kill -0 "$qpid" 2>/dev/null || break
    sleep 0.1
done

screendump() {   # $1 = output .ppm
    python3 - "$SOCK" "$1" <<'PY'
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
cmd("qmp_capabilities")
r = cmd("screendump", filename=sys.argv[2])
if "error" in r: print("[gop] screendump error:", r["error"]); sys.exit(1)
PY
}

if [ "$got" -eq 1 ] && grep -aq '\[fb\] gop pattern drawn' "$LOG"; then
    screendump "$DUMP"
    # Arm C: let the ring-3 compositor come up on the same framebuffer through
    # sys_fb_map's physical-address mapping (the line DDR-1142 changed), then
    # dump the scanout again.
    for _ in $(seq 1 $((TIMEOUT_S * 10))); do
        grep -aq 'PRADYOS_AMBIANCE_OK' "$LOG" 2>/dev/null && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    grep -aq 'PRADYOS_AMBIANCE_OK' "$LOG" && sleep 1 && screendump "$DUMP2"
fi
kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$SOCK"

fail=0
line=$(grep -a '^\[fb\] gop ' "$LOG" | grep -v 'pattern drawn' | head -1 | tr -d '\r')
echo "[gop] kernel: ${line:-<no [fb] gop line>}"
if [ "$line" = "$EXPECT_G" ]; then echo "[gop] arm G PASS"
else echo "[gop] arm G FAIL -- want '$EXPECT_G'"; fail=1; fi

if [ -s "$DUMP" ]; then
    if python3 - "$DUMP" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
# binary PPM: P6\n<w> <h>\n<max>\n<rgb...>
parts = d.split(b'\n', 3)
assert parts[0] == b'P6', parts[0]
w, h = map(int, parts[1].split()); body = parts[3]
want = {"TL": (255, 0, 0), "TR": (0, 255, 0), "BL": (0, 0, 255), "BR": (255, 255, 0)}
pts = {"TL": (w // 4, h // 4), "TR": (3 * w // 4, h // 4),
       "BL": (w // 4, 3 * h // 4), "BR": (3 * w // 4, 3 * h // 4)}
bad = 0
print(f"[gop] screendump {w}x{h}")
for k, (x, y) in pts.items():
    o = (y * w + x) * 3; px = tuple(body[o:o + 3])
    ok = px == want[k]; bad |= not ok
    print(f"[gop]   {k} ({x},{y}) = {px} want {want[k]} {'ok' if ok else 'MISMATCH'}")
sys.exit(1 if bad else 0)
PY
    then echo "[gop] arm S PASS"; else echo "[gop] arm S FAIL"; fail=1; fi
else
    echo "[gop] arm S FAIL -- no screendump (pattern never drawn)"; fail=1
fi

if [ -s "$DUMP2" ]; then
    if python3 - "$DUMP2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3); w, h = map(int, parts[1].split()); body = parts[3]
pattern = {(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)}
pts = [(w // 4, h // 4), (3 * w // 4, h // 4), (w // 4, 3 * h // 4), (3 * w // 4, 3 * h // 4)]
bad = 0
for (x, y) in pts:
    o = (y * w + x) * 3; px = tuple(body[o:o + 3])
    still = px in pattern; bad |= still
    print(f"[gop]   desktop ({x},{y}) = {px} {'STILL THE KERNEL PATTERN' if still else 'changed'}")
sys.exit(1 if bad else 0)
PY
    then echo "[gop] arm C PASS"; else echo "[gop] arm C FAIL -- compositor pixels never reached the scanout"; fail=1; fi
else
    echo "[gop] arm C FAIL -- compositor never reached PRADYOS_AMBIANCE_OK"; fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "[gop] capture tail:"; tail -20 "$LOG"
fi

# Arm B: the BIOS path. boot_test.sh exits early once the sentinel appears.
if TIMEOUT_S=90 EXTRA_SENTINEL='[fb] gop none' \
   FORBIDDEN_SENTINEL='[fb] gop rejected' \
   SERIAL_LOG="$OUT/gop_bios.log" bash tools/qemu_runner/boot_test.sh "$IMG" >"$OUT/gop_bios.out" 2>&1; then
    echo "[gop] arm B PASS"
else
    echo "[gop] arm B FAIL"; tail -15 "$OUT/gop_bios.out"; fail=1
fi

[ "$fail" -eq 0 ] && echo "[gop] PASS (G, S, C, B)" || { echo "[gop] FAIL"; exit 1; }
