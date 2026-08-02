#!/usr/bin/env bash
# tools/ci/start_align_check.sh — DDR-823. Every user/ _start is stack-aligned.
#
# SysV enters a process with RSP 16-byte ALIGNED (it points at argc), and
# kernel/exec/elf.c does exactly that. A compiler treats `_start` as an ordinary
# function, which is entered via `call` with RSP ≡ 8 (mod 16). So _start's own
# frame is off by 8, every callee inherits it, and the first 16-byte-aligned
# stack access raises #GP (not #AC — which is why it reads like a protection
# bug, not an alignment one).
#
# The fix is __attribute__((force_align_arg_pointer)) on _start. It was applied
# to all 23 existing probes — but a fix applied by hand to a directory is
# exactly the class of thing this project has now been bitten by three times:
#
#   DDR-817  ci.yml's gate list drifted from the Makefile  -> 8 gates never ran
#   DDR-822  the Makefile's source list drifted from user/ -> 14 stale probes
#   here     a per-file attribute that a new probe can omit
#
# In all three the failure is SILENT and looks like success. So this is asserted
# rather than remembered: a new probe without the attribute fails the build.
#
# Why a lint and not a compiler flag: -mstackrealign would apply the prologue to
# EVERY function in every probe, paying the cost ~200 times to fix one entry
# point. The attribute is correct exactly where the ABI mismatch is.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
rc=0
missing=""

for f in "$ROOT"/user/*.c; do
    grep -qE '(^|[^_[:alnum:]])_start[[:space:]]*\(' "$f" || continue
    # The attribute must be on the _start declaration itself. Checked within the
    # 3 lines preceding it so a force_align_arg_pointer elsewhere in the file
    # cannot satisfy this by accident.
    if ! grep -B3 -E 'void[[:space:]]+_start[[:space:]]*\(' "$f" \
         | grep -q 'force_align_arg_pointer'; then
        missing="$missing ${f#"$ROOT"/}"
        rc=1
    fi
done

if [ $rc -ne 0 ]; then
    echo "start-align-check: FAIL — _start without force_align_arg_pointer:" >&2
    for f in $missing; do echo "  $f" >&2; done
    cat >&2 <<'MSG'
start-align-check: a process entry point is entered with RSP 16-byte aligned,
                   but the compiler assumes the call convention's RSP ≡ 8 (mod
                   16). Without the attribute the frame is off by 8 and the
                   first aligned SSE stack access #GPs — which may not happen
                   until the probe's code changes, so "it works today" is not
                   evidence. Add:

                     __attribute__((noreturn, force_align_arg_pointer))
                     void _start(void) { ... }
MSG
    exit 1
fi

n=$(grep -lE 'void[[:space:]]+_start[[:space:]]*\(' "$ROOT"/user/*.c | wc -l)
echo "start-align-check: OK — $n user/ entry points, all stack-aligned"
