#!/usr/bin/env python3
"""TEMPORARY execve diagnostic. Removed before any commit."""
import io
BS = chr(92); NL = '"' + BS + 'r' + BS + 'n"'
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/syscall/sys_exec.c"; s = rd(p)
assert "execve-diag" not in s, "already instrumented"
if '#include "console.h"' not in s:
    i = s.index('#include'); j = s.index(chr(10), i) + 1
    s = s[:j] + '#include "console.h"   /* TEMP execve-diag */\n' + s[j:]

s = sub(s, "    if (vfs_open(t->fs_cap, t->root_mnt, kpath, &vf) != 0)\n        return -ENOENT;",
        "    if (vfs_open(t->fs_cap, t->root_mnt, kpath, &vf) != 0) {\n"
        "        kputs(\"[execve-diag] exit ENOENT path=\"); kputs(kpath); kputs(" + NL + ");\n"
        "        return -ENOENT;\n    }", "ENOENT")
s = sub(s, "    if (vf.size == 0 || vf.size > EXEC_MAX)\n        return -ENOEXEC;",
        "    if (vf.size == 0 || vf.size > EXEC_MAX) {\n"
        "        kputs(\"[execve-diag] exit ENOEXEC vf.size=\"); kputdec((uint64_t)vf.size);\n"
        "        kputs(\" EXEC_MAX=\"); kputdec((uint64_t)EXEC_MAX); kputs(" + NL + ");\n"
        "        return -ENOEXEC;\n    }", "ENOEXEC")
s = sub(s, "    if (!buf)\n        return -ENOMEM;",
        "    if (!buf) {\n"
        "        kputs(\"[execve-diag] exit ENOMEM isize=\"); kputdec((uint64_t)isize); kputs(" + NL + ");\n"
        "        return -ENOMEM;\n    }", "ENOMEM")
s = sub(s, "    if (vfs_read(t->fs_cap, &vf, 0, buf, isize) != (int)isize) {\n        kfree(buf);\n        return -EIO;",
        "    {\n        int _rd = vfs_read(t->fs_cap, &vf, 0, buf, isize);\n"
        "        if (_rd != (int)isize) {\n"
        "        kputs(\"[execve-diag] exit EIO isize=\"); kputdec((uint64_t)isize);\n"
        "        kputs(\" read=\"); kputdec((uint64_t)(_rd < 0 ? 0 : _rd)); kputs(" + NL + ");\n"
        "        kfree(buf);\n        return -EIO;", "EIO")
s = sub(s, "        return -EIO;\n    }\n", "        return -EIO;\n        }\n    }\n", "EIO-close")
wr(p, s); print("execve diagnostics added")
