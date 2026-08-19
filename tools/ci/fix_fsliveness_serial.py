#!/usr/bin/env python3
"""smoke-fs-liveness must KEEP its serial capture.

boot_test.sh cats the serial log only on the FAILURE path; on PASS it prints a
one-line verdict and deletes the capture. For an ordinary gate that is correct.
For a DIAGNOSTIC target whose only product is the per-iteration [bsp] trace, it
destroys the evidence -- and an empty result then reads as "the instrument is
silent" when in fact it was never recorded (STOP-2).

boot_test.sh honours SERIAL_LOG from the environment, so pin it to a known path
and print the churn trace explicitly afterwards, on pass AND fail.
"""
import io
BS = chr(92)
TAB = chr(9)
p = "Makefile"
s = io.open(p, encoding="utf-8", newline="").read()

old = (TAB + "TIMEOUT_S=120 EXTRA_SENTINEL=\"$$(printf '")
i = s.index("smoke-fs-liveness:")
j = s.index(old, i)
k = s.index("exit $$rc", j) + len("exit $$rc")
recipe = s[j:k]

new_recipe = recipe.replace(
    TAB + "TIMEOUT_S=120 EXTRA_SENTINEL=",
    TAB + "SERIAL_LOG=$(BUILD_DIR)/gatelogs/fsliveness_serial.log " + BS + "\n"
    + TAB + "    TIMEOUT_S=120 EXTRA_SENTINEL=", 1)

new_recipe = new_recipe.replace(
    TAB + "  echo \"[bsp-liveness] last churn iteration reached:\" ; " + BS + "\n",
    TAB + "  echo \"[bsp-liveness] serial kept at $(BUILD_DIR)/gatelogs/fsliveness_serial.log\" ; " + BS + "\n"
    + TAB + "  echo \"[bsp-liveness] churn trace (last 3):\" ; " + BS + "\n"
    + TAB + "  grep -a 'churn iter=' $(BUILD_DIR)/gatelogs/fsliveness_serial.log | tail -3 || echo '[bsp-liveness] NO [bsp] LINES - flag not compiled in' ; " + BS + "\n"
    + TAB + "  grep -ac 'btree churn OK' $(BUILD_DIR)/gatelogs/fsliveness_serial.log | sed 's/^/[bsp-liveness] btree-churn-OK count: /' ; " + BS + "\n", 1)

assert new_recipe != recipe, "no substitution made"
s = s[:j] + new_recipe + s[k:]
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("smoke-fs-liveness now keeps its serial capture")
