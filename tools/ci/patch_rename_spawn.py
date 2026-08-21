#!/usr/bin/env python3
"""DDR-956: incbin the probe and spawn it under probe_enabled("renametest")."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "arch/x86_64/user_image.asm"; s = rd(p)
assert "renametest" not in s, "already incbin'd"
s = sub(s, "global stackdemand_elf\n",
        "; DDR-956: ring-3 SYS_RENAME probe\n"
        "global renametest_elf\n"
        "global renametest_elf_end\n"
        "renametest_elf:\n"
        '    incbin "build/renametest.elf"\n'
        "renametest_elf_end:\n\n"
        "global stackdemand_elf\n", "incbin")
wr(p, s); print("incbin added")

p = "kernel/main.c"; s = rd(p)
assert "renametest_elf" not in s, "already declared"
s = sub(s, "extern const unsigned char stackdemand_elf[];",
        "extern const unsigned char renametest_elf[];          /* DDR-956: SYS_RENAME probe */\n"
        "extern const unsigned char renametest_elf_end[];\n"
        "extern const unsigned char stackdemand_elf[];", "extern")

# Spawn beside the ftruncate probe, which also needs the SFS root mount.
anchor = '                if (probe_enabled("ftruncate")) {'
spawn = ('                /* DDR-956: SYS_RENAME probe. Opt-in like every other probe so\n'
         '                 * it does not add a process to every unrelated gate\'s log.\n'
         '                 * root_mnt is set to the SFS volume BEFORE unblock, matching the\n'
         '                 * ftruncate probe below -- the probe creates and renames files, so\n'
         '                 * it needs a writable root, and a thread must not be runnable\n'
         '                 * before its root is in place. */\n'
         '                if (probe_enabled("renametest")) {\n'
         '                    struct tcb *rp = 0;\n'
         '                    uint64_t rlen = (uint64_t)(renametest_elf_end - renametest_elf);\n'
         '                    if (elf_load((void *)(uintptr_t)renametest_elf, rlen,\n'
         '                                 "RENAMETEST", &rp) == ELF_OK && rp) {\n'
         '                        rp->root_mnt = smnt;          /* SFS root before unblock */\n'
         '                        sched_unblock(rp);\n'
         '                        kputs("[user] ELF loaded (embedded); rename probe spawned\r\n");\n'
         '                    } else {\n'
         '                        kputs("[user] rename probe FAILED to load\r\n");\n'
         '                    }\n'
         '                }\n' + anchor)
s = sub(s, anchor, spawn, "spawn")
wr(p, s); print("spawn added")
