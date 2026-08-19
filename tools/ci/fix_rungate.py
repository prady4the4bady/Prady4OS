import io
p = "tools/ci/run_gate.sh"
s = io.open(p, encoding="utf-8", newline="").read()
old = 'grep -aE "^PASS |^\[smoke\] FAIL|^FAIL|required pattern|forbidden" "$log" | head -8\n'
assert s.count(old) == 1
# The runner previously missed "[smoke] PASS" and so reported NO verdict line
# for a gate that had plainly passed -- the same class of error as trusting an
# empty grep (T3): a missing match is not a verdict.
new = 'grep -aE "^PASS |^\[smoke\] (PASS|FAIL)|^FAIL|required pattern|forbidden" "$log" | head -8\n'
s = s.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("run_gate verdict pattern fixed")
