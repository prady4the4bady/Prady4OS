#!/usr/bin/env bash
# wheel_inject.sh — mouse-wheel injector for the smoke-scroll gate (DDR-725).
# Waits for the readiness sentinel ($3) in the serial log ($1), then drives the
# virtio-tablet's wheel through QMP `input-send-event` (btn wheel-up/wheel-down)
# on the unix socket ($2) — the genuine hardware path (EV_REL/REL_WHEEL).
#   $1 = serial log   $2 = QMP unix socket   $3 = readiness sentinel
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_FOCUS}"

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
if s is not None:
    f = s.makefile("rw")
    f.readline()                                 # QMP greeting
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        f.readline()
    cmd({"execute": "qmp_capabilities"})
    # Alternate directions over several rounds; each press+release is one detent.
    for _round in range(6):
        for b in ("wheel-down", "wheel-up"):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": b, "down": True}}]}})
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": b, "down": False}}]}})
            time.sleep(0.3)
PY
