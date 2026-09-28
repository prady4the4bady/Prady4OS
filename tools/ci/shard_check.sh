#!/usr/bin/env bash
# tools/ci/shard_check.sh — DDR-817. Every gate is in exactly one CI shard.
#
# The failure mode this exists for: a gate that is in the Makefile and in no
# shard. CI goes green faster, and the reason it is faster is that it stopped
# running something — a green that means less than the green it replaced.
#
# That is not hypothetical. Before DDR-817, `ci.yml` hand-listed 111 gate steps
# and nothing compared that list to the Makefile; eight gates had never run in
# CI at all, including the gate for every crypto primitive that had been
# promoted to main on "two CI greens".
#
# Exits non-zero, naming the offending targets, if any smoke-* target is
# unassigned or assigned more than once. Host-only, no QEMU, ~instant.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="$ROOT/tools/ci/gate_shards.txt"
MAKEFILE="$ROOT/Makefile"

# Targets deliberately NOT in the shard matrix. Each carries its reason: an
# unexplained exclusion is exactly how a gate goes missing, and a blanket
# "every target must be sharded" would be silently satisfiable by deleting the
# target instead of fixing it.
#
#   smoke-aarch64  ) run by the `arch-bootstrap` matrix job, not build-and-boot
#   smoke-riscv64  )
#   smoke-selftest ) DDR-785 self-tests the boot harness itself. It must run
#                    BEFORE any gate that trusts the harness, so it is a setup
#                    step in EVERY shard rather than one gate in one shard.
#   (smoke-sfs-btree-smp4 was excluded here as DDR-824's OPEN-10 reproduction
#    surface, "register it when OPEN-10 is fixed". OPEN-10 IS fixed -- DDR-964
#    named the mechanism, a create-then-init race making cap_ok return -EPERM,
#    and mutation-checked the fix at 8 sites -- so DDR-1061 REGISTERED it on
#    shard 5. Note what the condition asked for: a fixed MECHANISM, not a
#    bounded rate. DDR-1061 §2 costs out the rate campaign and shows it cannot
#    settle the question at any N reachable here -- n=44 merely REACHES the
#    historical 6.7%, at 182 s a run, foreground-only. Do not run it.)
#   smoke-agent-live ) developer-run only: needs a live Ollama endpoint on the
#                      host, so CI stays in test mode (ADR-027)
# smoke-fs-liveness: DDR-777/BUG-1 diagnostic. It REBUILDS the kernel with
# BSP_LIVENESS=1, whose per-iteration churn output fills the 4 KiB dmesg ring
# and evicts smoke-dmesg's required marker (DDR-790). It must never run in the
# shared-image shard matrix. Remove this exclusion when BUG-1 closes and the
# target is deleted.
# smoke-fast (directive §6.3) is NOT a gate — it is a RUNNER that invokes some
# other gate COUNT times via tools/ci/campaign.sh. It boots nothing of its own,
# so assigning it to a shard would make CI run an argument-less campaign that
# immediately errors out. Excluded as infrastructure, not as a skipped test.
EXCLUDE="smoke-aarch64 smoke-riscv64 smoke-agent-live smoke-selftest smoke-fs-liveness smoke-fast"

excluded() {
    local t="$1" e
    for e in $EXCLUDE; do [ "$t" = "$e" ] && return 0; done
    return 1
}

[ -f "$MANIFEST" ] || { echo "shard-check: missing $MANIFEST" >&2; exit 1; }

# Manifest targets (field 2 of the tab-separated, non-comment lines).
assigned="$(awk -F'\t' '/^[0-9]/ && NF>=3 { print $2 }' "$MANIFEST")"

rc=0

# 1. Duplicates within the manifest.
dupes="$(printf '%s\n' "$assigned" | sort | uniq -d)"
if [ -n "$dupes" ]; then
    echo "shard-check: FAIL — target assigned to more than one shard:" >&2
    printf '  %s\n' $dupes >&2
    rc=1
fi

# 2. Every smoke-* target in the Makefile is assigned or explicitly excluded.
missing=""
while read -r t; do
    [ -n "$t" ] || continue
    excluded "$t" && continue
    printf '%s\n' "$assigned" | grep -qx "$t" || missing="$missing $t"
done < <(grep -oE '^smoke-[a-z0-9-]+:' "$MAKEFILE" | tr -d ':' | sort -u)

if [ -n "$missing" ]; then
    echo "shard-check: FAIL — Makefile gate(s) in no shard and not excluded:" >&2
    for t in $missing; do echo "  $t" >&2; done
    echo "shard-check: add each to tools/ci/gate_shards.txt, or to EXCLUDE in" >&2
    echo "             this script WITH the reason. Do not delete the gate." >&2
    rc=1
fi

# 3. Every manifest target actually exists in the Makefile — a typo in the
#    manifest would otherwise mean a shard silently runs one gate fewer.
while read -r t; do
    [ -n "$t" ] || continue
    grep -qE "^$t:" "$MAKEFILE" || {
        echo "shard-check: FAIL — manifest names a target the Makefile does not define: $t" >&2
        rc=1
    }
done < <(printf '%s\n' "$assigned" | sort -u)

if [ $rc -eq 0 ]; then
    n="$(printf '%s\n' "$assigned" | grep -c .)"
    s="$(printf '%s\n' "$EXCLUDE" | wc -w)"
    echo "shard-check: OK — $n gates assigned across $(awk -F'\t' '/^[0-9]/{print $1}' "$MANIFEST" | sort -u | wc -l) shards, $s excluded with reasons"
fi
exit $rc
