#!/usr/bin/env python3
"""Revert FEAT-E's behavioural change. Keeps the _rooted helper (used by the
wrapper, and correct infrastructure for the eventual fix); drops only the PRISM
rooting and the SFS EXECTEST placement that depended on it."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)

p = "kernel/main.c"; s = rd(p)

old_prism = ('                /* DDR-957 (FEAT-E): PRISM roots at the SFS volume. Its ELF was\n'
             '                 * already loaded FROM there; without this it resolved every\n'
             '                 * path against the FAT default, where fat32 has no rename op. */\n'
             '                struct tcb *pr = user_boot_from_sfs_rooted(cap, smnt, "PRISM.ELF",\n'
             '                                                    prism_elf, prism_elf_end, 0, smnt);')
assert old_prism in s, "prism rooting"
new_prism = ('                /* DDR-957: PRISM stays on the FAT default root. Rooting it at\n'
             '                 * `smnt` was tried and reverted -- that volume is REFORMATTED at\n'
             '                 * sfs_format(sbd) later in boot, and smoke-shell asserts on\n'
             '                 * FAT-only fixtures (/HELLO.TXT, /BIG8K.TXT, /EXECTEST.ELF).\n'
             '                 * See DDR-957 sec.9-10 before trying again. */\n'
             '                struct tcb *pr = user_boot_from_sfs(cap, smnt, "PRISM.ELF",\n'
             '                                                    prism_elf, prism_elf_end, 0);')
s = s.replace(old_prism, new_prism, 1)

i = s.index('                /* DDR-957 follow-on: PRISM now resolves')
j = s.index('                fat_place_exec_image(cap, smnt);\n', i) + len('                fat_place_exec_image(cap, smnt);\n')
s = s[:i] + s[j:]
wr(p, s); print("FEAT-E behaviour reverted; _rooted helper retained")
