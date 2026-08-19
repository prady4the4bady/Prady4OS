#!/usr/bin/env python3
"""Add smoke-blk-timeout target + shard registration.

R2: every backslash is built with chr(92) so none traverses the heredoc and
collapses into a real control character (that defect has bitten twice already).
"""
import io
BS = chr(92); TAB = chr(9)

p = "Makefile"; s = io.open(p, encoding="utf-8", newline="").read()
assert "smoke-blk-timeout" not in s, "already added"

anchor = "smoke-fs: $(IMG) fat-image sfs-image\n"
assert s.count(anchor) == 1, "smoke-fs target anchor"
end = s.index("\n\n", s.index(anchor))

extra = ("msix vec=56" + BS + "n"
         "PRADYOS filesystem works!" + BS + "n"
         "[pdrive] workspace OK")
forbid = ("[vblk] compl wait timeout" + BS + "n"
          "[vblk] slot wait timeout")

block = ("\n\n# DDR-955: proves the bounded-wait path is present and that its deadline does\n"
         "# NOT fire on healthy I/O. The FORBIDDEN sentinels are the discriminating half\n"
         "# (R8): a spurious 500-tick expiry turns a passing boot into a failure here,\n"
         "# which is the regression this change could plausibly introduce.\n"
         "smoke-blk-timeout: $(IMG) fat-image sfs-image\n"
         + TAB + "TIMEOUT_S=120 EXTRA_SENTINEL=\"$$(printf '" + extra + "')\" " + BS + "\n"
         + TAB + "    FORBIDDEN_SENTINEL=\"$$(printf '" + forbid + "')\" " + BS + "\n"
         + TAB + "    bash tools/qemu_runner/boot_test.sh $(IMG)")
s = s[:end] + block + s[end:]
s = s.replace(".PHONY: smoke-fs-liveness", ".PHONY: smoke-blk-timeout smoke-fs-liveness", 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("Makefile target added")

p = "tools/ci/gate_shards.txt"; s = io.open(p, encoding="utf-8", newline="").read()
assert "smoke-blk-timeout" not in s, "shard already registered"
if not s.endswith(chr(10)): s += chr(10)
s += "0" + TAB + "smoke-blk-timeout" + TAB + "120" + chr(10)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("shard registered")
