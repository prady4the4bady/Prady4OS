#!/usr/bin/env bash
# input_inject.sh — for the smoke-input gate (DDR-703). Waits until the ring-3
# reader announces PRADYOS_INPUT_WAIT in the serial log ($1), then injects real
# key presses through QEMU's HMP monitor on the unix socket ($2) via `sendkey`,
# so the emulated i8042 raises IRQ1 (the genuine hardware path).
set -u
log="$1"
sock="$2"

for _ in $(seq 1 600); do
    grep -q PRADYOS_INPUT_WAIT "$log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.5

python3 - "$sock" <<'PY'
import socket, sys, time
sock = sys.argv[1]
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
    # Fire the key several times; the reader exits on the first byte it sees.
    for _ in range(12):
        try:
            s.sendall(b"sendkey a\n")
        except OSError:
            break
        time.sleep(0.25)
PY
