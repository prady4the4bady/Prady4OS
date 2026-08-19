#!/usr/bin/env python3
"""KEEP_SERIAL opt-in for boot_test.sh.

The serial capture is deleted on EVERY exit path -- there are TEN `rm -f
"$SERIAL_LOG"` sites, not the two a summary suggested. Wrapping each one
individually would be ten chances to miss one, and a missed site silently
destroys the evidence again. So define ONE helper and route every call through
it: the opt-in then cannot be partially applied.

Default behaviour is unchanged (KEEP_SERIAL unset => still removed), so ordinary
gates are untouched and no gate starts leaking temp files.
"""
import io, re
p = "tools/qemu_runner/boot_test.sh"
s = io.open(p, encoding="utf-8", newline="").read()

assert "serial_rm()" not in s, "already patched"

# 1) Define the helper immediately after SERIAL_LOG is established.
anchor = 'SERIAL_LOG="${SERIAL_LOG:-$(mktemp)}"\n'
assert s.count(anchor) == 1, "SERIAL_LOG anchor"
helper = anchor + '''
# DDR-777 / BUG-1 diagnostics: KEEP_SERIAL=1 preserves the serial capture.
#
# Every exit path below removes the capture, which is correct for a normal gate
# but destroys the only product of a diagnostic run (the [bsp] churn trace).
# Pinning SERIAL_LOG from the environment is NOT enough -- the file is deleted
# regardless of where it lives, which is why three validation attempts produced
# an empty capture and looked like "the instrument is silent".
#
# One helper, used by every removal site, so the opt-in cannot be applied to
# some paths and missed on others. Unset (the default) behaves exactly as before.
serial_rm() {
    [ -n "${KEEP_SERIAL:-}" ] && return 0
    rm -f "$@"
}
'''
s = s.replace(anchor, helper, 1)

# 2) Route every removal through it. Count first so the result is verifiable.
before = len(re.findall(r'(?<!serial_)rm -f "\$SERIAL_LOG"', s))
s = re.sub(r'(?<!serial_)rm -f "\$SERIAL_LOG"', 'serial_rm "$SERIAL_LOG"', s)
after = len(re.findall(r'(?<!serial_)rm -f "\$SERIAL_LOG"', s))

io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("keepserial: rewrote %d removal sites, %d remaining" % (before, after))
