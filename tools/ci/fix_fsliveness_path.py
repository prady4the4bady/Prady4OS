import io
p = "Makefile"
s = io.open(p, encoding="utf-8", newline="").read()
# BUILD_DIR is build/toolchain, NOT build -- the serial path must be the same
# build/gatelogs/ directory every other harness uses (and which exists).
n = s.count("$(BUILD_DIR)/gatelogs/fsliveness_serial.log")
assert n >= 1, "path token not found"
s = s.replace("$(BUILD_DIR)/gatelogs/fsliveness_serial.log",
              "build/gatelogs/fsliveness_serial.log")
# Make the absent case LOUD: grep on a missing/empty file printed nothing and the
# || fallback did not fire, so the "no trace" case was silent -- the very outcome
# the fallback was added to prevent. Test the file explicitly instead.
old = ("  grep -a 'churn iter=' build/gatelogs/fsliveness_serial.log | tail -3 "
       "|| echo '[bsp-liveness] NO [bsp] LINES - flag not compiled in' ; ")
new = ("  if [ ! -s build/gatelogs/fsliveness_serial.log ]; then "
       "echo '[bsp-liveness] SERIAL CAPTURE MISSING OR EMPTY - harness broken, not a verdict'; "
       "elif grep -aq 'churn iter=' build/gatelogs/fsliveness_serial.log; then "
       "grep -a 'churn iter=' build/gatelogs/fsliveness_serial.log | tail -3; "
       "else echo '[bsp-liveness] NO [bsp] LINES - BSP_LIVENESS not compiled in'; fi ; ")
assert old in s, "trace block not found"
s = s.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("path + absent-case reporting fixed")
