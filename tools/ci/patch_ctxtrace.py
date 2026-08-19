#!/usr/bin/env python3
"""STEP 1-A instrument: print every sfs_ctx birth and death.

R2: every backslash is built with chr(92). Nothing containing a literal
backslash traverses the Windows->WSL->heredoc->python chain, which collapses
\r\n into real CR/LF and leaves C string literals unterminated.
"""
import io
BS = chr(92)
NL = '"' + BS + 'r' + BS + 'n"'          # the C literal "\r\n"
p = "kernel/fs/sfs/sfs.c"
s = io.open(p, encoding="utf-8", newline="").read()

old_um = """static void sfs_umount(void *ctx) {
    kfree(ctx);
}"""
assert s.count(old_um) == 1, "umount site"
new_um = ("""static void sfs_umount(void *ctx) {
    /* DDR-953 STEP 1-A: record every ctx death so the stale pointer seen by
     * sfs_bd_guard can be matched against the ctx that was freed, and by whom.
     * Paired with the [sfs] mount line below. Diagnostic only. */
    kputs("[sfs] umount ctx="); kputhex((uint64_t)(uintptr_t)ctx);
    kputs(" caller="); kputhex((uint64_t)(uintptr_t)__builtin_return_address(0));
    kputs(""" + NL + """);
    kfree(ctx);
}""")
s = s.replace(old_um, new_um, 1)

old_mt = "    *ctx = c;"
assert s.count(old_mt) == 1, "mount out site"
new_mt = ("""    /* DDR-953 STEP 1-A: record every ctx birth (pointer + its bd). */
    kputs("[sfs] mount ctx="); kputhex((uint64_t)(uintptr_t)c);
    kputs(" bd=");             kputhex((uint64_t)(uintptr_t)c->bd);
    kputs(" caller=");         kputhex((uint64_t)(uintptr_t)__builtin_return_address(0));
    kputs(""" + NL + """);
    *ctx = c;""")
s = s.replace(old_mt, new_mt, 1)

io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("ctxtrace: patched sfs_mount + sfs_umount")
