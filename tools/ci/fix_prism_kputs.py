import io
BS = chr(92)
p = "kernel/main.c"
s = io.open(p, encoding="utf-8", newline="").read()
bad = 'kputs("[user] PRISM rooted at clean SFS root; unblocked" ' + BS + 'r' + BS + 'n");'
assert bad in s, "malformed line not found"
good = 'kputs("[user] PRISM rooted at clean SFS root; unblocked' + BS + 'r' + BS + 'n");'
s = s.replace(bad, good, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("kputs repaired")
