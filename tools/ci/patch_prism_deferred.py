#!/usr/bin/env python3
"""DDR-957 STEP 2: load PRISM BLOCKED, root at root_smnt after the reformat,
unblock only then -- mirroring the AETHER daemon (main.c:1969 / 2092-2093)."""
import io
BS = chr(92); NL = '"' + BS + 'r' + BS + 'n"'
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/main.c"; s = rd(p)
assert "DDR-957 STEP2" not in s, "already applied"

old = ('                struct tcb *pr = user_boot_from_sfs(cap, smnt, "PRISM.ELF",\n'
       '                                                    prism_elf, prism_elf_end, 0);')
i = s.index(old)
# keep the preceding comment block but replace the call
new = ('                /* DDR-957 STEP2: load PRISM BLOCKED and root it at the CLEAN\n'
       '                 * persistent SFS root, mirroring the AETHER daemon below.\n'
       '                 * Rooting at `smnt` is unfixable: sfs_format(sbd) reformats that\n'
       '                 * very volume later in boot, so anything PRISM could open is\n'
       '                 * wiped underneath it. Hand-rolled elf_load (DDR-739 pattern)\n'
       '                 * because user_boot_from_sfs unblocks internally, and root_mnt\n'
       '                 * must be set BEFORE the first run (main.c:1831). */\n'
       '                struct tcb *pr = 0;\n'
       '                {\n'
       '                    uint64_t plen2 = (uint64_t)(prism_elf_end - prism_elf);\n'
       '                    if (elf_load((void *)(uintptr_t)prism_elf, plen2,\n'
       '                                 "PRISM", &pr) != ELF_OK)\n'
       '                        pr = 0;\n'
       '                }')
s = s[:i] + new + s[i+len(old):]

# Root + unblock beside the daemon's, after root_smnt exists.
old_dm = ("                            dm->root_mnt = (prov_mnt >= 0) ? prov_mnt : root_smnt;\n"
          "                            sched_unblock(dm);")
new_dm = ("                        /* DDR-957 STEP2: PRISM gets the same treatment -- rooted at\n"
          "                         * the clean volume, then unblocked. Order matters. */\n"
          "                        if (pr) {\n"
          "                            pr->root_mnt = root_smnt;\n"
          "                            sched_unblock(pr);\n"
          "                            kputs(\"[user] PRISM rooted at clean SFS root; unblocked\" " + NL[1:] + ");\n"
          "                        }\n"
          + old_dm)
s = sub(s, old_dm, new_dm, "prism unblock")
wr(p, s); print("PRISM deferred-unblock applied")
