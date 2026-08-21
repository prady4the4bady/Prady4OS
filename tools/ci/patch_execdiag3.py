import io
BS = chr(92); NL = '"' + BS + 'r' + BS + 'n"'
p = "kernel/syscall/sys_exec.c"
s = io.open(p, encoding="utf-8", newline="").read()
assert "elf_build_image rc" not in s
old = "    if (rc != ELF_OK)\n        return -ENOEXEC;"
assert s.count(old) == 1
new = ("    if (rc != ELF_OK) {\n"
       "        kputs(\"[execve-diag] elf_build_image rc=\"); kputdec((uint64_t)rc);\n"
       "        kputs(\" isize=\"); kputdec((uint64_t)isize); kputs(" + NL + ");\n"
       "        return -ENOEXEC;\n    }")
s = s.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("elf_build_image diagnostic added")
