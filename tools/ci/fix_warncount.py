import io
p = "tools/ci/build_check.sh"
s = io.open(p, encoding="utf-8", newline="").read()
old = 'warns=$(grep -ci "warning" "$LOG")\n'
assert s.count(old) == 1
# make emits its own "warning:" lines (clock skew from Windows/WSL mtime drift).
# Those are not compiler diagnostics and must not trip -Werror accounting.
new = ('warns=$(grep -i "warning" "$LOG" | grep -cvE "^make(\[[0-9]+\])?: ")\n')
s = s.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("warn filter fixed")
