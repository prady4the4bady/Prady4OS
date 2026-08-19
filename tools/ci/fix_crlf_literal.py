import io
BS = chr(92)                      # a real backslash, never passed through the shell
p = "kernel/fs/sfs/sfs.c"
s = io.open(p, encoding="utf-8", newline="").read()

broken = '    kputs("\r\n");\n'    # literal CR LF inside the string literal
if broken not in s:
    broken = '    kputs("\n");\n'
assert broken in s, "broken kputs not found"

fixed = '    kputs("' + BS + 'r' + BS + 'n");\n'
s = s.replace(broken, fixed, 1)
io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("fixed:", repr(fixed))
