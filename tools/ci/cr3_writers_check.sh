#!/usr/bin/env bash
# tools/ci/cr3_writers_check.sh — DDR-1077.
#
# DDR-1075 §3 measured that this kernel has NO cross-CPU TLB invalidation at
# all, and that the absence is CORRECT today on four facts. The third is the
# fragile one: NO TWO THREADS SHARE AN ADDRESS SPACE. Every ->cr3 assignment in
# the tree installs a FRESH address space or the kernel master, so a CPU never
# holds a translation for an address space another CPU is mutating.
#
# Ship threads that share one (Group D's pthread / clone(CLONE_VM) row) and
# every vmm_unmap (sys_mmap.c:67, sys_surface.c:388/:448) and every
# vmm_protect_range leaves a stale writable translation on another CPU --
# silent, timing-dependent, on the same SMP paths OPEN-2 lives in. DDR-1075 §3.2
# recorded that NOTHING IN THE TREE WOULD NOTICE: no assertion, no counter, no
# gate. The warning lived only in a document, and DDR-1071 §4 measured what that
# produces -- a row whose work finished cleanly and drew no follow-up is exactly
# the row that stays stale.
#
# This does NOT build a shootdown. It builds the noticing.
#
# WHY ->cr3 AND NOT CLONE_VM: grepping for CLONE_VM pins a SPELLING, not the
# property (DDR-1073 §5's line-number lesson one level up). A future session
# could share an address space through any name -- but every way of sharing one
# must assign a cr3 somewhere. So the pin is the SET OF cr3 ASSIGNMENT SITES,
# which is the actual carrier of the premise.
#
# PINNED PER FILE AND COUNT, NEVER BY LINE NUMBER -- DDR-1073 §5 is explicit
# that a row citing line numbers has an expiry date and nothing in the tree can
# check one. This check must not acquire one.
#
# THE CONDITION IS A CONJUNCTION AND THAT IS THE WHOLE DESIGN:
#
#     FAIL  iff  (the cr3-writer set differs from the pin)
#           AND  (no cross-CPU TLB invalidation exists in the tree)
#
# Either term alone is wrong in a way this project has already been bitten by.
# "A new cr3 writer is banned" reddens on CORRECT work -- a session that builds
# the shootdown FIRST and then CLONE_VM would be blocked by the very check meant
# to protect that ordering, which is DDR-1071 §5's refused shape. "A shootdown
# must exist" fails TODAY, on a tree where the absence is correct, so it would
# be silenced on day one. The conjunction permits every correct state and
# forbids exactly the dangerous one.
#
# PLUS ONE UNCONDITIONAL CLAUSE (DDR-1077 §3.3, found while implementing §6's
# fixture 5): ZERO cr3 writers is ALWAYS a failure, shootdown or not. Zero does
# not mean the tree changed, it means THE MEASUREMENT BROKE -- the
# GLOBAL_FORBIDDEN catastrophe at 89f71cc (§NON-NEGOTIABLE 6), where an empty
# list failed nothing and just stopped catching. Under the bare conjunction a
# broken pattern plus any file containing the word "shootdown" would have
# reported success.
#
# IT IS NOT A VERDICT. It cannot tell whether a new cr3 writer actually shares
# an address space -- that is semantic, and DDR-1071 §5 / DDR-1072 §2 both
# established that nothing in the tree can read a semantic claim. It says the
# thing DDR-1075 §3.1(c) rests on has moved; go and look.
set -uo pipefail

TREE_ROOT="${TREE_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"

# The pin: <relative path> <count>, sorted. Measured 2026-09-06, not carried.
PIN=$(cat <<'PINEOF'
kernel/exec/elf.c 1
kernel/proc/sched.c 2
kernel/syscall/sys_exec.c 1
PINEOF
)

cd "$TREE_ROOT" || { echo "cr3-writers-check: FAIL — no tree at $TREE_ROOT" >&2; exit 2; }

# Weak by design, and the weakness is in the SAFE direction: an assignment
# spelled some other way is a cr3 writer this misses, which leaves the check
# firing on the sites it does see. It never manufactures a pass.
hits=$(grep -rnE '(->|\.)cr3[[:space:]]*=[^=]' kernel user \
         --include='*.c' --include='*.h' 2>/dev/null || true)

if [ -n "$hits" ]; then
    actual=$(printf '%s\n' "$hits" | cut -d: -f1 | sort | uniq -c \
             | awk '{print $2, $1}' | sort)
    total=$(printf '%s\n' "$hits" | wc -l)
else
    actual=""
    total=0
fi

# "A shootdown exists" is the same measurement DDR-1075 §3 used to establish the
# absence. If someone builds one under another name the check keeps firing on
# new cr3 writers: annoying, not dangerous. Stated rather than discovered later.
if grep -rniq 'shootdown' kernel user 2>/dev/null; then
    shootdown=1
else
    shootdown=0
fi

# Clause 1 — the measurement itself. Unconditional; see the header.
if [ "$total" -eq 0 ]; then
    cat >&2 <<'MSG'
cr3-writers-check: FAIL — ZERO cr3 assignment sites found.

That is not a tree with no address spaces in it; it is a BROKEN PATTERN. A
check whose grep stops matching must not report success -- that is the
GLOBAL_FORBIDDEN catastrophe at 89f71cc (§NON-NEGOTIABLE 6), where an empty
list failed nothing and simply stopped catching, unnoticed for four commits.

Fix the pattern in tools/ci/cr3_writers_check.sh, do not adjust the pin.
MSG
    exit 1
fi

if [ "$actual" = "$PIN" ]; then
    echo "cr3-writers-check: OK — $total cr3 assignment sites across 3 files, set unchanged (shootdown=$shootdown)"
    exit 0
fi

# Clause 2 — the conjunction.
if [ "$shootdown" -eq 1 ]; then
    echo "cr3-writers-check: OK — cr3 writer set CHANGED, and a cross-CPU TLB"
    echo "                   invalidation exists in the tree, which is the correct"
    echo "                   ordering (prerequisite first, then the sharing)."
    echo "                   Update the pin in tools/ci/cr3_writers_check.sh."
    printf 'expected:\n%s\nactual:\n%s\n' "$PIN" "$actual"
    exit 0
fi

cat >&2 <<'MSG'
cr3-writers-check: FAIL — the cr3 assignment sites have changed and this tree
still has NO cross-CPU TLB invalidation.

This is NOT a verdict that anything is broken. It is DDR-1077's tripwire: the
premise DDR-1075 §3.1(c) rests on -- that NO TWO THREADS SHARE AN ADDRESS
SPACE -- is carried entirely by the set of cr3 assignment sites, and that set
has moved. Go and look at the new site.

  * If it installs a FRESH address space or the kernel master (as all four
    pinned sites do), the premise still holds. Update the pin below and say so
    in the commit message.

  * If it lets two runnable threads share one cr3, THE PREMISE IS GONE, and
    every vmm_unmap (sys_mmap.c, sys_surface.c) and vmm_protect_range now
    leaves a stale writable translation on another CPU -- silent,
    timing-dependent, on the same SMP paths OPEN-2 lives in. A TLB shootdown is
    a PREREQUISITE of that change, not a Phase 9 optimisation (DDR-1075 §3.2).
    Build it first.
MSG
printf 'expected:\n%s\nactual:\n%s\n' "$PIN" "$actual" >&2
exit 1
