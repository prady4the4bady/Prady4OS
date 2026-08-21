import io
BS = chr(92)
p = "kernel/main.c"
s = io.open(p, encoding="utf-8", newline="").read()
good = BS + 'r' + BS + 'n'
n = 0
for msg in ("[user] ELF loaded (embedded); rename probe spawned",
            "[user] rename probe FAILED to load"):
    for broken in ('kputs("' + msg + '\r\n");', 'kputs("' + msg + '\n");'):
        if broken in s:
            s = s.replace(broken, 'kputs("' + msg + good + '");', 1)
            n += 1
            break
assert n == 2, "expected 2 repairs, made %d" % n
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("repaired", n, "kputs literals")
