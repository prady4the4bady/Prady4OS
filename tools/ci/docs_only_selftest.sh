#!/usr/bin/env bash
# docs_only_selftest.sh — DDR-1187 §4. Each arm feeds docs_only.sh a fixed
# path list and checks its answer. Every arm but A expects `false`: those are
# the inputs on which a wrong `true` would skip the boot gates for a change
# that needs them. Mutants M1-M5 in the DDR each turn exactly one arm red.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
C="$ROOT/tools/ci/docs_only.sh"
fail=0
arm() {  # arm <name> <want> <paths...>
    local name="$1" want="$2"; shift 2
    local got
    got="$(printf '%s\n' "$@" | bash "$C")"
    if [ "$got" = "$want" ]; then
        echo "docs_only selftest: $name ok ($got)"
    else
        echo "docs_only selftest: FAIL $name got=$got want=$want"
        fail=1
    fi
}
arm A_docs_only       true  docs/ddr/DDR-1.md CLAUDE.md docs/a/b/c.md
arm B_kernel          false kernel/proc/sched.c
arm C_empty           false ""
arm D_docs_then_code  false docs/ddr/DDR-1.md SESSION_HANDOFF.md user/shmtest.c
arm E_iso_legal       false docs/legal/EULA.txt
arm F_root_non_md     false Makefile
arm G_workflow        false .github/workflows/ci.yml
arm H_nested_md       false kernel/README.md
if [ "$fail" -ne 0 ]; then echo "docs_only selftest: FAILED"; exit 1; fi
echo "docs_only selftest: all 8 arms ok"
