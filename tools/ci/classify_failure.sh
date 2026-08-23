#!/usr/bin/env bash
# tools/ci/classify_failure.sh — match a failing serial log against every
# ALREADY-CHARACTERIZED failure signature (operator directive §6.2).
#
# WHY THIS EXISTS. This project's recorded cost is not diagnosis, it is
# RE-diagnosis: OPEN-10 was re-derived across sessions, DDR-880 unified two
# issues on a shared signature and DDR-884 had to undo it, and B#3 was chased
# through three wrong subsystems (virtio-blk, MSI-X delivery, the LAPIC) before
# DDR-981 named the cause. Each of those started with someone reading a red log
# with no index of what was already known.
#
# Output is deliberately one of two things:
#   "MATCH <name> — do not re-diagnose, see <ref>"   or   "NEW SIGNATURE"
# A match is a pointer to a decision already made; it is NOT permission to
# ignore the failure. A gate that is red is still red.
#
# Usage: tools/ci/classify_failure.sh <serial-or-gate-log> [...]
set -u

# name | ref | grep -F pattern
# Ordered most-specific first: [apfreeze] must win over the block symptoms it
# causes, or a recurrence of B#3 would be filed under its own downstream effect.
SIGS=(
"AP freeze (B#3 / OPEN-2 root cause)|DDR-981|[apfreeze]"
"OPEN-13 kheap double-free|DDR-980 §2|KHEAP PANIC: kfree: double free"
"OPEN-12 ring-0 panic|DDR-979|component: NEXUS isr"
"B#3 downstream: completion timeout|DDR-981 §7|[vblk] compl wait timeout"
"OPEN-10 B+tree churn (create-then-init race)|DDR-964|btree churn FAIL"
"Item 48 workers-late|DDR-966|reason=workers-late"
"virtio-blk checksum mismatch (driver bug — NOT scheduling)|§INV.2|reason=checksum-mismatch"
"FSRM umount-under-live-probe|DDR-967|created file did not persist"
"smoke-agents preempt frozen|DDR-968|PRADYOS_AGENT_WITNESS_WAIT"
"FAT32 multi-cluster (REFUTED — needs a fresh artefact to reopen)|DDR-973|FAT32MC FAIL"
"OPEN-1 smoke-surfdestroy intermittent|OPEN-1|SURFDESTROY FAIL"
"multi-inflight block failure|DDR-981 §7|multi-inflight FAIL"
"blk integrity failure|DDR-981 §7|blk integrity FAIL"
"resched failure|DDR-981 §7 (was OPEN-2)|resched FAIL"
"msix-on-AP failure|OPEN-2 (NOT claimed closed — does no block I/O)|msix on AP FAIL"
"rqstress failure|DDR-939|rqstress FAIL"
)

rc=0
for log in "$@"; do
    [ -f "$log" ] || { echo "classify: no such file: $log" >&2; rc=1; continue; }
    echo "=== $log ==="
    hit=0
    for sig in "${SIGS[@]}"; do
        IFS='|' read -r name ref pat <<< "$sig"
        if grep -qaF -- "$pat" "$log"; then
            echo "  MATCH: $name"
            echo "     pattern: $pat"
            echo "     ref:     $ref  — characterized already; do NOT re-diagnose from scratch."
            grep -aF -m2 -- "$pat" "$log" | sed 's/^/     | /'
            hit=1
            break          # most-specific wins; see the ordering note above
        fi
    done
    if [ "$hit" = 0 ] && ! grep -qaE "FAIL|PANIC|\\[BUG\\]|forbidden pattern|timeout" "$log"; then
        # Distinguish "a failure nobody has characterized" from "not a failure".
        # Reporting a clean log as NEW SIGNATURE would send someone hunting a
        # defect that is not there — the mirror of the re-diagnosis waste this
        # script exists to stop.
        echo "  CLEAN — no failure indicators in this log at all."
        hit=1
    fi
    if [ "$hit" = 0 ]; then
        echo "  NEW SIGNATURE — no characterized failure matches. Investigate and,"
        echo "  once characterized, add it to SIGS in this script so the next"
        echo "  session does not start over."
        grep -anE "FAIL|PANIC|BUG|timeout|Traceback" "$log" 2>/dev/null | head -5 | sed 's/^/     | /'
        rc=2
    fi
done
exit $rc
