#!/usr/bin/env python3
"""DDR-956: two PRISM-driven rename gates. No new blob, no size cost."""
import io
TAB = chr(9); BS = chr(92)
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)

p = "Makefile"; s = rd(p)
assert "smoke-rename" not in s, "already added"
NL = BS + 'n'

def gate(name, cmds, checks, doc):
    r = doc + name + ": $(IMG) fat-image sfs-image\n"
    r += TAB + '@echo "[' + name + '] booting PRISM; feeding commands at the prompt..."\n'
    r += (TAB + '@# FIFO must live on a real Linux fs -- DrvFs (/mnt/c) has no FIFOs.\n')
    r += (TAB + '@SHIN=$$(mktemp -u /tmp/pradyos_' + name + '.XXXXXX); '
          'rm -f build/' + name + '.log; mkfifo "$$SHIN"; ' + BS + '\n')
    r += (TAB + '( exec > "$$SHIN"; ' + BS + '\n')
    r += (TAB + '  for i in $$(seq 1 300); do grep -q PRISM_READY build/' + name
          + '.log 2>/dev/null && break; sleep 0.1; done; ' + BS + '\n')
    for c in cmds:
        r += TAB + "  printf '" + c + NL + "'; sleep 0.6; " + BS + "\n"
    r += TAB + "  printf 'exit" + NL + "'; sleep 0.5 ) & " + BS + "\n"
    r += (TAB + 'timeout 60 qemu-system-x86_64 -M q35 ' + BS + '\n'
          + TAB + '    -drive if=none,format=raw,file=$(IMG),id=disk0 -device virtio-blk-pci,drive=disk0,bootindex=0 ' + BS + '\n'
          + TAB + '    -drive if=none,format=raw,file=$(FAT_IMG),id=disk1 -device virtio-blk-pci,drive=disk1 ' + BS + '\n'
          + TAB + '    -drive if=none,format=raw,file=$(SFS_IMG),id=disk2 -device virtio-blk-pci,drive=disk2 ' + BS + '\n'
          + TAB + '    -serial stdio -display none -monitor none < "$$SHIN" > build/' + name + '.log 2>/dev/null || true; ' + BS + '\n'
          + TAB + 'rm -f "$$SHIN"\n')
    for expr, msg in checks:
        r += (TAB + '@' + expr + ' || { echo "[' + name + '] FAIL: ' + msg
              + '"; tail -40 build/' + name + '.log; exit 1; }\n')
    r += TAB + '@echo "[' + name + '] PASS"\n\n'
    return r

ok_doc = ("# DDR-956: SYS_RENAME through the PRISM shell. No embedded probe blob --\n"
          "# kernel.bin has only ~3.7 KiB of headroom under the 1 MiB stage-2 window\n"
          "# (DDR-733/827), and PRISM is already in the image.\n"
          "#\n"
          "# The content check is the strong arm. A rename that creates a NEW empty\n"
          "# entry instead of moving the inode would still let /RENDST.TXT open, so\n"
          "# 'the destination exists' proves nothing. Requiring the ORIGINAL bytes to\n"
          "# come back out proves the inode was carried across. The marker is counted\n"
          "# >=2 because PRISM echoes the command that wrote it, so a single match\n"
          "# would be satisfied by the echo alone -- a sentinel that fires regardless\n"
          "# of whether rename worked (R8).\n")
ok_checks = [
    ('test "$$(grep -c renmark7k2 build/smoke-rename.log)" -ge 2',
     'content did not survive the rename (inode not carried across)'),
    ('grep -q "mv: /RENSRC.TXT -> /RENDST.TXT" build/smoke-rename.log',
     'mv did not report success'),
    ('grep -q "cat: cannot open /RENSRC.TXT" build/smoke-rename.log',
     'source still readable after rename (copy-style, not a rename)'),
]
s_ok = gate("smoke-rename",
            ["echo renmark7k2 > /RENSRC.TXT",
             "mv /RENSRC.TXT /RENDST.TXT",
             "cat /RENDST.TXT",
             "cat /RENSRC.TXT"],
            ok_checks, ok_doc)

en_doc = ("# DDR-956: the stub-catcher. A handler that returns 0 unconditionally passes\n"
          "# every positive arm; only this one fails it. Renaming an absent path MUST\n"
          "# report failure, and the success line MUST NOT appear.\n")
en_checks = [
    ('grep -q "mv: cannot rename /NOSUCH999.TXT" build/smoke-rename-enoent.log',
     'rename of an absent path did not report failure'),
    ('! grep -q "mv: /NOSUCH999.TXT ->" build/smoke-rename-enoent.log',
     'rename of an absent path reported SUCCESS (stub behaviour)'),
]
s_en = gate("smoke-rename-enoent",
            ["mv /NOSUCH999.TXT /RENX.TXT"],
            en_checks, en_doc)

anchor = "smoke-blk-timeout: $(IMG) fat-image sfs-image\n"
i = s.index(anchor)
i = s.rindex("# DDR-955", 0, i)
s = s[:i] + s_ok + s_en + s[i:]
s = s.replace(".PHONY: smoke-blk-timeout",
              ".PHONY: smoke-rename smoke-rename-enoent smoke-blk-timeout", 1)
wr(p, s); print("two gates added")

p = "tools/ci/gate_shards.txt"; s = rd(p)
assert "smoke-rename" not in s
if not s.endswith(chr(10)): s += chr(10)
s += "4" + TAB + "smoke-rename" + TAB + "60" + chr(10)
s += "5" + TAB + "smoke-rename-enoent" + TAB + "60" + chr(10)
wr(p, s); print("shards 4 and 5 registered")
