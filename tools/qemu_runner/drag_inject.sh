#!/usr/bin/env bash
# drag_inject.sh — window-drag injector for the smoke-drag gate (DDR-710). Waits
# for a readiness sentinel ($3) in the serial log ($1), then drives a drag via
# QEMU QMP input-send-event on the unix socket ($2): move onto window B's title
# bar, press, drag to a new location (button held), release.
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_AMBIANCE_OK}"

for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

SOCK="$sock" python3 - <<'PY'
import os, socket, json, time
sock = os.environ["SOCK"]
s = None
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        break
    except OSError:
        s = None
        time.sleep(0.2)
if s is None:
    raise SystemExit(0)

f = s.makefile("rw", newline="\n")
f.readline()                                    # QMP greeting
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush(); f.readline()
def absmove(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}]}})
def btn(down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": down}}]}})

cmd({"execute": "qmp_capabilities"})
# Window B's title bar is around pixel (160,130) -> abs (5120,5546); drag target
# pixel (400,350) -> abs (12800,14933). Repeat so a missed phase still lands.
for _round in range(3):
    absmove(5120, 5546);  time.sleep(0.35)      # over B's title bar
    btn(True);            time.sleep(0.45)       # press (start drag)
    absmove(12800, 14933); time.sleep(0.45)      # drag (button held)
    btn(False);           time.sleep(0.6)        # release (drop)
PY
