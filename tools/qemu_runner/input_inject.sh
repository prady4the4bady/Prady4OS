#!/usr/bin/env bash
# input_inject.sh — keyboard injector for the smoke-input / smoke-compositor gates.
# Waits until a readiness sentinel ($3) appears in the serial log ($1), then injects
# real key presses through QEMU's HMP monitor on the unix socket ($2) via `sendkey`,
# so the emulated i8042 raises IRQ1 (the genuine hardware path).
#   $1 = serial log path   $2 = HMP unix socket
#   $3 = readiness sentinel (default PRADYOS_INPUT_WAIT)
#   $4 = space-separated keys to send, each several times (default "a")
set -u
log="$1"
sock="$2"
sentinel="${3:-PRADYOS_INPUT_WAIT}"
keys="${4:-a}"

for _ in $(seq 1 600); do
    grep -q "$sentinel" "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

KEYS="$keys" SOCK="$sock" python3 - <<'PY'
import os, socket, time
sock = os.environ["SOCK"]
keys = os.environ["KEYS"].split()
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
    # Send each key a few times (the reader/compositor acts on the first byte of
    # each); repeat the whole sequence so a missed keystroke still lands.
    for _round in range(4):
        for k in keys:
            try:
                s.sendall(("sendkey %s\n" % k).encode())
            except OSError:
                break
            time.sleep(0.25)
PY
