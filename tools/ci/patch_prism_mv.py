#!/usr/bin/env python3
"""DDR-956 step 2f: PRISM mv builtin. Single-line anchors only."""
import io
BS = chr(92)
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "user/prism.c"; s = rd(p)
assert '"mv"' not in s, "mv already present"
NL = BS + 'n'          # the two-character C escape, never a real newline (R2)

anchor = '        } else if (!strcmp(cmd, "rm")) {'
block = ('        } else if (!strcmp(cmd, "mv")) {\n'
         '            /* DDR-956: rename within the shell root mount. Both paths go to\n'
         '             * SYS_RENAME unchanged -- there is no cross-mount form to reject,\n'
         '             * because a process only ever sees t->root_mnt. */\n'
         '            if (argc < 3) printf("mv: usage: mv <old> <new>' + NL + '");\n'
         '            else if (nsi(SYS_RENAME, (long)argv[1], (long)argv[2], 0) == 0)\n'
         '                printf("mv: %s -> %s' + NL + '", argv[1], argv[2]);\n'
         '            else\n'
         '                printf("mv: cannot rename %s' + NL + '", argv[1]);\n'
         + anchor)
s = sub(s, anchor, block, "mv builtin")

s = sub(s, "setname touch rm uname", "setname touch rm mv uname", "help text")

if "#define SYS_RENAME" not in s:
    a2 = "#define SYS_UNLINK"
    assert s.count(a2) >= 1, "SYS_UNLINK define"
    s = s.replace(a2, "#define SYS_RENAME  95   /* DDR-956 */\n" + a2, 1)

wr(p, s); print("mv builtin + help + SYS_RENAME added")
