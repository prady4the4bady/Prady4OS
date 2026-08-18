#!/usr/bin/env python3
"""sym_at.py <hex-addr> [elf] -- name the function containing an address.

Why not awk: awk does arithmetic in IEEE doubles. Kernel addresses like
0xFFFFFFFF8002CD18 are ~1.8e19, far past the 2^53 exact-integer limit, so every
comparison silently loses precision and the "nearest symbol <= addr" answer is
wrong -- it returned a symbol whose base was ABOVE the target. Python integers
are arbitrary precision, so this is exact.
"""
import subprocess, sys, os

addr = int(sys.argv[1], 16)
elf = sys.argv[2] if len(sys.argv) > 2 else "build/kernel.elf"
if not os.path.exists(elf):
    sys.exit("sym_at: %s missing" % elf)

out = subprocess.run(["nm", "-C", "--defined-only", "-n", elf],
                     capture_output=True, text=True).stdout

syms = []
for line in out.splitlines():
    f = line.split()
    if len(f) >= 3 and f[1].lower() in ("t", "w"):     # text symbols only
        try:
            syms.append((int(f[0], 16), " ".join(f[2:])))
        except ValueError:
            pass
syms.sort()

prev = None
for a, n in syms:
    if a <= addr:
        prev = (a, n)
    else:
        nxt = (a, n)
        break
else:
    nxt = None

if prev is None:
    sys.exit("sym_at: no text symbol at or below 0x%x" % addr)
print("sym_at: 0x%x is in %s (base 0x%x, offset +0x%x)" % (addr, prev[1], prev[0], addr - prev[0]))
if nxt:
    print("sym_at: next symbol %s at 0x%x -- containing function spans 0x%x..0x%x"
          % (nxt[1], nxt[0], prev[0], nxt[0]))
    if addr >= nxt[0]:
        print("sym_at: WARNING address is past the next symbol")
