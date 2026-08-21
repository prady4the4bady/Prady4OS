#!/usr/bin/env python3
"""DDR-957 follow-on: place /EXECTEST.ELF on SFS too, now that PRISM roots there."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)

p = "kernel/main.c"; s = rd(p)
assert s.count('fat_place_exec_image(cap, smnt)') == 0, "already placed on SFS"

# The PRISM launch site is the right place: smnt is in scope and the shell is
# about to be rooted there.
old = ('                /* DDR-957 (FEAT-E): PRISM roots at the SFS volume.')
assert s.count(old) == 1, "prism anchor"
new = ('                /* DDR-957 follow-on: PRISM now resolves `run <path>` against SFS,\n'
       '                 * but /EXECTEST.ELF was only ever placed on the FAT32 root\n'
       '                 * (fat_place_exec_image at the mnt call site). smoke-shell\n'
       '                 * backgrounds it twice and asserts Done(0); without a copy here it\n'
       '                 * exits 127 and the job-control assertions fail. The helper is\n'
       '                 * mount-generic, so the same bytes go onto the SFS volume. */\n'
       '                fat_place_exec_image(cap, smnt);\n'
       + old)
s = s.replace(old, new, 1)
wr(p, s); print("EXECTEST.ELF also placed on SFS")
