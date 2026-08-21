import io
BS = chr(92); NL = '"' + BS + 'r' + BS + 'n"'
p = "kernel/syscall/sys_exec.c"
s = io.open(p, encoding="utf-8", newline="").read()
assert "execve-entry" not in s
old = ("    struct tcb *t = current_thread;\n"
       "    if (t->root_mnt < 0)\n"
       "        return -ENOENT;                        /* no filesystem to exec from */\n")
assert s.count(old) == 1
new = ("    struct tcb *t = current_thread;\n"
       "    kputs(\"[execve-entry] root_mnt=\"); kputdec((uint64_t)(t->root_mnt < 0 ? 9999 : t->root_mnt));\n"
       "    kputs(" + NL + ");\n"
       "    if (t->root_mnt < 0) {\n"
       "        kputs(\"[execve-diag] exit EARLY root_mnt<0\"); kputs(" + NL + ");\n"
       "        return -ENOENT;\n    }\n")
s = s.replace(old, new, 1)
old2 = ("    if (plen < 0)\n"
        "        return (long)plen;                     /* -EFAULT or -ENAMETOOLONG */\n")
assert s.count(old2) == 1
new2 = ("    if (plen < 0) {\n"
        "        kputs(\"[execve-diag] exit EARLY copyinstr plen=\"); kputdec((uint64_t)(-plen)); kputs(" + NL + ");\n"
        "        return (long)plen;\n    }\n")
s = s.replace(old2, new2, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("entry + early-exit diagnostics added")
