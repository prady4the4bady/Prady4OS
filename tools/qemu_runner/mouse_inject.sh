#!/usr/bin/env bash
# mouse_inject.sh — pointer injector for the smoke-mouse gate (DDR-705). Waits for
# a readiness sentinel ($3, default PRADYOS_COMPOSITOR_OK) in the serial log ($1),
# then drives the virtio-tablet through QEMU's QMP `input-send-event` on the unix
# socket ($2): an absolute move followed by a left-button click — the real
# virtio-input path. (HMP has no portable absolute-pointer command; QMP does.)
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_COMPOSITOR_OK}"

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
f.readline()                                   # QMP greeting

def cmd(obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    f.readline()                               # response (ignored)

cmd({"execute": "qmp_capabilities"})

# Repeat the move+click a few times so a missed event still lands.
for _round in range(5):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": 16000}},
        {"type": "abs", "data": {"axis": "y", "value": 12000}},
    ]}})
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]}})
    time.sleep(0.2)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": False}},
    ]}})
    time.sleep(0.3)
PY
