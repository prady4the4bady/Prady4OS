#!/usr/bin/env python3
"""DDR-957: root PRISM at SFS by setting root_mnt BEFORE sched_unblock."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/main.c"; s = rd(p)
assert "user_boot_from_sfs_rooted" not in s, "already patched"

# 1) Rename the body and add the parameter.
s = sub(s,
    "static struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,\n"
    "                               const unsigned char *elf, const unsigned char *elf_end,\n"
    "                               int sovereign) {",
    "/* DDR-957: `root_mnt >= 0` selects the new thread's root mount BEFORE it is\n"
    " * unblocked. That ordering is the whole point -- this helper unblocks\n"
    " * internally, so a caller assigning ->root_mnt on the returned pointer would\n"
    " * race a thread that is already runnable (see main.c:1831). */\n"
    "static struct tcb *user_boot_from_sfs_rooted(cap_t cap, int smnt, const char *fname,\n"
    "                               const unsigned char *elf, const unsigned char *elf_end,\n"
    "                               int sovereign, int root_mnt) {", "definition")

# 2) Apply it next to is_sovereign, before sched_unblock.
s = sub(s,
    "        if (sovereign)\n"
    "            ut->is_sovereign = 1;      /* authority BEFORE the first run */\n"
    "        sched_unblock(ut);             /* elf_load returns the thread BLOCKED */",
    "        if (sovereign)\n"
    "            ut->is_sovereign = 1;      /* authority BEFORE the first run */\n"
    "        if (root_mnt >= 0)\n"
    "            ut->root_mnt = root_mnt;   /* DDR-957: root BEFORE the first run */\n"
    "        sched_unblock(ut);             /* elf_load returns the thread BLOCKED */", "apply")

# 3) Thin wrapper keeps all 25 existing call sites unchanged.
anchor = "/* DDR-957: `root_mnt >= 0` selects"
wrapper = ("/* Unchanged entry point for the ~25 callers that keep the default root. */\n"
           "static struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,\n"
           "                               const unsigned char *elf, const unsigned char *elf_end,\n"
           "                               int sovereign);\n\n")
s = s.replace(anchor, wrapper + anchor, 1)

# definition of the wrapper goes right after the rooted body's closing brace:
end_marker = "    return 0;\n}\n"
i = s.index("static struct tcb *user_boot_from_sfs_rooted")
j = s.index(end_marker, i) + len(end_marker)
s = (s[:j] +
     "\nstatic struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,\n"
     "                               const unsigned char *elf, const unsigned char *elf_end,\n"
     "                               int sovereign) {\n"
     "    return user_boot_from_sfs_rooted(cap, smnt, fname, elf, elf_end, sovereign, -1);\n"
     "}\n" + s[j:])

# 4) PRISM roots at SFS.
s = sub(s,
    '                struct tcb *pr = user_boot_from_sfs(cap, smnt, "PRISM.ELF",\n'
    '                                                    prism_elf, prism_elf_end, 0);',
    '                /* DDR-957 (FEAT-E): PRISM roots at the SFS volume. Its ELF was\n'
    '                 * already loaded FROM there; without this it resolved every\n'
    '                 * path against the FAT default, where fat32 has no rename op. */\n'
    '                struct tcb *pr = user_boot_from_sfs_rooted(cap, smnt, "PRISM.ELF",\n'
    '                                                    prism_elf, prism_elf_end, 0, smnt);', "prism")
wr(p, s); print("FEAT-E applied")
