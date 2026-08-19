import io
BS = chr(92)
p = "kernel/drivers/blk/virtio_blk.c"
s = io.open(p, encoding="utf-8", newline="").read()
good = '"' + BS + 'r' + BS + 'n"'
n = 0
for msg in ("[vblk] slot wait timeout", "[vblk] compl wait timeout"):
    broken = 'kputs("' + msg + '\r\n");'
    if broken not in s:
        broken = 'kputs("' + msg + '\n");'
    assert broken in s, "not found: " + msg
    s = s.replace(broken, 'kputs("' + msg + BS + 'r' + BS + 'n");', 1)
    n += 1
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("fixed", n, "kputs literals")
