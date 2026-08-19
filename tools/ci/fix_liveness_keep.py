import io
p = "Makefile"
s = io.open(p, encoding="utf-8", newline="").read()
old = "\tSERIAL_LOG=build/gatelogs/fsliveness_serial.log \\n"
assert s.count(old) == 1, "serial line"
s = s.replace(old, "\tKEEP_SERIAL=1 SERIAL_LOG=build/gatelogs/fsliveness_serial.log \\n", 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("smoke-fs-liveness now sets KEEP_SERIAL=1")
