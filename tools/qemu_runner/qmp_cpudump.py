#!/usr/bin/env python3
"""DDR-887: dump every vCPU's register state over QMP just before a gate times out.

WHY THIS EXISTS
---------------
`g_ticks` intermittently stops advancing under -smp 4 (DDR-887). The deduced
mechanism is a system-wide spinlock deadlock: `spin_lock_irqsave()` masks
interrupts and THEN spins, so one CPU wedged while holding a lock parks every
other contender with IF=0, and no timer interrupt is delivered on any CPU.

Nothing inside the guest can report that. `kputs` itself takes the console lock
and masks interrupts, so any in-kernel tracer is unreachable by construction once
the wedge happens. The observation has to come from OUTSIDE the guest.

`info registers -a` gives RIP and RFLAGS for all vCPUs. Four CPUs sitting at a
`pause` inside `spin_lock_irqsave` with IF clear confirms the deadlock, and the
RIPs name the lock sites directly (resolve with `llvm-addr2line -e
build/kernel.elf <rip>`).

usage: qmp_cpudump.py <unix-socket-path> <output-file>
"""
import json
import socket
import sys
import time


def qmp(sock, cmd, args=None):
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    sock.sendall((json.dumps(msg) + "\n").encode())
    # Replies interleave with async events; return the first non-event object.
    buf = b""
    deadline = time.time() + 10
    while time.time() < deadline:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not line.strip():
                continue
            obj = json.loads(line)
            if "event" in obj:          # async, not our reply
                continue
            return obj
    return None


def main():
    if len(sys.argv) != 3:
        print("usage: qmp_cpudump.py <socket> <outfile>", file=sys.stderr)
        return 2
    sock_path, out_path = sys.argv[1], sys.argv[2]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.recv(65536)                        # greeting
    qmp(s, "qmp_capabilities")

    lines = ["=== DDR-887 QMP vCPU dump (taken before timeout kill) ==="]
    for cmd in ("info cpus", "info registers -a"):
        r = qmp(s, "human-monitor-command", {"command-line": cmd})
        lines.append("--- %s ---" % cmd)
        lines.append((r or {}).get("return", "<no reply>"))
    s.close()

    with open(out_path, "a") as f:
        f.write("\n".join(str(x) for x in lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
