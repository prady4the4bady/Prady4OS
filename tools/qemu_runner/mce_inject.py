#!/usr/bin/env python3
"""mce_inject.py — inject a machine check through QMP once the guest is ready.

DDR-1044. Waits for the guest to print its CR4.MCE report (so the injection
cannot land before machine checks can be DELIVERED — without CR4.MCE, QEMU
raises a triple fault instead and the guest dies with no diagnostic at all),
then injects a known bank-0 record through the human monitor.

READINESS IS POLLED, NOT SLEPT. A fixed sleep is a guess about boot time that
is wrong on a loaded CI runner in whichever direction hurts; DDR-910 made the
same correction for the pointer injector.

The injected values are FIXED and the gate asserts them back out of the guest's
decode, so "the kernel printed some numbers" and "the kernel decoded THESE
numbers" stay different claims:

    status  0xBD80000000000000   VAL|UC|EN|MISCV|ADDRV
    addr    0x1234
    misc    0x8C
    mcgstat 0x5                  RIPV|MCIP

usage: mce_inject.py <qmp-sock> <serial-log> [ready-timeout-s]
"""
import json, os, socket, sys, time

READY = "PRADYOS_MCE cpuid="

def qmp(s, cmd, args=None):
    m = {"execute": cmd}
    if args:
        m["arguments"] = args
    s.sendall((json.dumps(m) + "\n").encode())
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            return {"error": "qmp closed"}
        buf += chunk
        for line in buf.split(b"\n"):
            if not line.strip():
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            if "return" in d or "error" in d:
                return d


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    sock_path, log_path = sys.argv[1], sys.argv[2]
    ready_timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0

    deadline = time.time() + ready_timeout
    while time.time() < deadline:
        try:
            with open(log_path, "r", errors="replace") as fh:
                if READY in fh.read():
                    break
        except OSError:
            pass
        time.sleep(0.25)
    else:
        # Say so rather than injecting blind: an injection before CR4.MCE is set
        # triple-faults, and the gate would then report "no #MC" about a kernel
        # that never got the chance.
        print("[mce_inject] NOT READY — '%s' never appeared within %.0fs; "
              "injecting anyway would triple-fault and blame the wrong thing"
              % (READY, ready_timeout))
        return 1

    for _ in range(40):
        if os.path.exists(sock_path):
            break
        time.sleep(0.25)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(20)
    s.connect(sock_path)
    s.recv(65536)
    qmp(s, "qmp_capabilities")
    r = qmp(s, "human-monitor-command",
            {"command-line": "mce 0 0 0xbd80000000000000 0x5 0x1234 0x8c"})
    print("[mce_inject] reply: %s" % json.dumps(r))
    s.close()
    # QEMU reports refusals in the RETURN string, not as a QMP error.
    return 1 if (r.get("return") or "").strip() else 0


if __name__ == "__main__":
    sys.exit(main())
