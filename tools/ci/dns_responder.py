#!/usr/bin/env python3
"""DDR-1141 sec.4 -- host-side DNS responder for smoke-dhcpdns.

Binds UDP 127.0.0.1:<port> (default 53). QEMU slirp forwards the guest's
traffic to its host alias (10.77.0.2 on the gate's network) to the host's
loopback, so this is what the guest reaches when it names 10.77.0.2 as its
resolver.

  * Every query name received is appended to LOG, one per line, BEFORE any
    reply is built. The gate's arm H reads that log: a name the kernel refused
    must NOT appear in it, because a refusal that still sends the packet has
    leaked the name, which is the thing privacy mode and the allowlist exist to
    stop. An audit record saying "denied" cannot show that; only the far end can.
  * *.pradyos.test answers A 10.77.0.99, a value the guest cannot invent.
  * Anything else answers NXDOMAIN.

Usage: dns_responder.py LOG [PORT]   (runs until killed)
"""
import socket
import struct
import sys

ANSWER = bytes([10, 77, 0, 99])
SUFFIX = ".pradyos.test"


def parse_qname(pkt, off):
    labels = []
    while off < len(pkt):
        n = pkt[off]
        off += 1
        if n == 0:
            return ".".join(labels), off
        if n & 0xC0 or off + n > len(pkt):
            return None, off
        labels.append(pkt[off:off + n].decode("ascii", "replace"))
        off += n
    return None, off


def reply(pkt, name, qend):
    tid = pkt[0:2]
    rd = pkt[2] & 0x01
    question = pkt[12:qend + 4]            # qname + qtype + qclass
    qtype = struct.unpack(">H", pkt[qend:qend + 2])[0] if qend + 2 <= len(pkt) else 0
    ok = name is not None and name.lower().endswith(SUFFIX) and qtype == 1
    flags = 0x8000 | (rd << 8) | 0x0080 | (0 if ok else 3)   # QR, RD echo, RA, NXDOMAIN
    hdr = tid + struct.pack(">HHHHH", flags, 1, 1 if ok else 0, 0, 0)
    body = question
    if ok:
        body += struct.pack(">HHHIH", 0xC00C, 1, 1, 60, 4) + ANSWER
    return hdr + body


def main():
    log_path = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 53
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    with open(log_path, "a", buffering=1) as log:
        log.write("# responder up on 127.0.0.1:%d\n" % port)
        while True:
            pkt, addr = s.recvfrom(1500)
            if len(pkt) < 17:
                continue
            name, qend = parse_qname(pkt, 12)
            log.write("%s\n" % (name if name is not None else "<malformed>"))
            s.sendto(reply(pkt, name, qend), addr)


if __name__ == "__main__":
    main()
