#!/usr/bin/env bash
# tools/ci/cr3_writers_selftest.sh — DDR-1077 §6.
#
# cr3_writers_check.sh passes on today's tree and will keep passing forever if
# nothing changes. That is the same shape as ci-probe-rodata-check and
# ci-start-align-check: a GUARD, whose liveness cannot be proved by watching it
# in production because a healthy tree never trips it. So it is proved here, on
# synthetic trees, and THE THREE THAT MUST FAIL ARE THE LOAD-BEARING HALF --
# without them "the check is quiet" and "the check is dead" are the same
# observation, which is the dead-arm class this project has now caught a dozen
# times.
#
# Fixture trees are built under build/ (§NON-NEGOTIABLE 7 -- never /tmp, WSL
# wipes it) and TREE_ROOT points the checker at them, so nothing here mutates
# the real tree.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK="$ROOT/tools/ci/cr3_writers_check.sh"
FIX="$ROOT/build/cr3fixtures"
rc=0

rm -rf "$FIX"
mkdir -p "$FIX"

# A synthetic tree carrying exactly the pinned set: elf.c 1, sched.c 2,
# sys_exec.c 1. The bodies are irrelevant to the check and are written as the
# real ones read, so a human opening a fixture sees why it is shaped this way.
mk_base() {
    local d="$1"
    mkdir -p "$d/kernel/exec" "$d/kernel/proc" "$d/kernel/syscall" "$d/kernel/mm" "$d/user"
    printf 'void f(void){ t->cr3 = as; }\n'                       > "$d/kernel/exec/elf.c"
    printf 'void g(void){ t->cr3 = 0; }\nvoid h(void){ t->cr3 = child_cr3; }\n' > "$d/kernel/proc/sched.c"
    printf 'void e(void){ t->cr3        = new_as; }\n'            > "$d/kernel/syscall/sys_exec.c"
    printf 'int  q(void){ return t->cr3 == 0; }\n'                > "$d/kernel/mm/vmm.c"
    printf 'int  main(void){ return 0; }\n'                       > "$d/user/probe.c"
}

expect() {                       # expect <name> <PASS|FAIL> <tree>
    local name="$1" want="$2" tree="$3" out got
    out=$(TREE_ROOT="$tree" bash "$CHECK" 2>&1)
    if [ $? -eq 0 ]; then got=PASS; else got=FAIL; fi
    if [ "$got" = "$want" ]; then
        echo "cr3-writers-selftest: $name — $got (expected $want) OK"
    else
        echo "cr3-writers-selftest: $name — $got but expected $want" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        rc=1
    fi
}

# 1 — the REAL tree. Without this the other five prove only that the checker
#     works on trees this file wrote; it would not prove the pin matches
#     reality, which is the one thing a stale pin would break.
expect "f1 real-tree" PASS "$ROOT"

# 2 — a 5th cr3 writer, no shootdown. THE DANGEROUS CASE; this is the point.
mk_base "$FIX/f2"
printf 'void thread_create(void){ t->cr3 = parent->cr3; }\n' > "$FIX/f2/kernel/proc/thread.c"
expect "f2 new-writer-no-shootdown" FAIL "$FIX/f2"

# 3 — the same 5th writer WITH a shootdown. The correct ordering (prerequisite
#     first, then the sharing) must be permitted, or the check blocks the very
#     work it exists to guard -- DDR-1071 §5's refused shape.
mk_base "$FIX/f3"
printf 'void thread_create(void){ t->cr3 = parent->cr3; }\n' > "$FIX/f3/kernel/proc/thread.c"
printf 'void tlb_shootdown(uint64_t va){ /* IPI every CPU on this AS */ }\n' > "$FIX/f3/kernel/mm/tlb.c"
expect "f3 new-writer-with-shootdown" PASS "$FIX/f3"

# 4 — vacuity. The pinned set with no shootdown is today's tree, and it must
#     pass; a check that simply always failed would satisfy 2, 5 and 6.
mk_base "$FIX/f4"
expect "f4 pinned-set-no-shootdown" PASS "$FIX/f4"

# 5 — ZERO writers. Not "a tree with no address spaces" but a BROKEN PATTERN,
#     and it must not report success: the GLOBAL_FORBIDDEN catastrophe at
#     89f71cc, where an empty list failed nothing and just stopped catching.
#     A shootdown is planted here deliberately, so this fixture also proves the
#     zero-guard is UNCONDITIONAL rather than riding on the conjunction
#     (DDR-1077 §3.3).
mk_base "$FIX/f5"
sed -i 's/->cr3/->pgdir/g' "$FIX/f5"/kernel/exec/elf.c "$FIX/f5"/kernel/proc/sched.c "$FIX/f5"/kernel/syscall/sys_exec.c
printf 'void tlb_shootdown(uint64_t va){ }\n' > "$FIX/f5/kernel/mm/tlb.c"
expect "f5 zero-writers-pattern-broke" FAIL "$FIX/f5"

# 6 — a writer MOVED between files; the TOTAL is still 4. Per-file counts are
#     what catch this, and M2 is the mutant that proves the distinction is not
#     decorative.
mk_base "$FIX/f6"
printf 'void g(void){ t->cr3 = 0; }\n'                          > "$FIX/f6/kernel/proc/sched.c"
printf 'void f(void){ t->cr3 = as; }\nvoid h(void){ t->cr3 = child_cr3; }\n' > "$FIX/f6/kernel/exec/elf.c"
expect "f6 writer-moved-between-files" FAIL "$FIX/f6"

if [ "$rc" -eq 0 ]; then
    echo "cr3-writers-selftest: OK — 3 must-pass + 3 must-fail fixtures"
fi
exit "$rc"
