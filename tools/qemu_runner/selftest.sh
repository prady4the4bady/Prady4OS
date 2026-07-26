#!/usr/bin/env bash
# DDR-785 — self-test for boot_test.sh's early exit. Host-only: no QEMU, no
# kernel. A stub `qemu-system-x86_64` on PATH writes a scripted serial log with
# controlled timing, so we can assert not just the verdict but WHEN the harness
# stops. Run via `make smoke-selftest`.
#
# The assertion that matters is #2: a forbidden pattern that arrives LATE must
# still fail the gate. That is the one way early exit could have silently
# weakened every gate, so it is tested directly rather than reasoned about.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT_TEST="$HERE/boot_test.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

# Stub qemu: ignores its arguments except -serial file:<path>, then replays
# STUB_SCRIPT ("<delay>|<line>" per entry) into that file. Sleeps at the end so
# the harness must decide to stop it; otherwise "early exit" is unfalsifiable.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/qemu-system-x86_64" <<'STUB'
#!/usr/bin/env bash
log=""
prev=""
for a in "$@"; do
    case "$prev" in -serial) log="${a#file:}" ;; esac
    prev="$a"
done
[ -n "$log" ] || exit 0
: > "$log"
IFS=';' read -ra steps <<< "${STUB_SCRIPT:-}"
for s in "${steps[@]}"; do
    [ -z "$s" ] && continue
    sleep "${s%%|*}"
    printf '%s\n' "${s#*|}" >> "$log"
done
sleep 300          # outlive any TIMEOUT_S: only an early exit can end this
STUB
chmod +x "$WORK/bin/qemu-system-x86_64"
export PATH="$WORK/bin:$PATH"

# A dummy image so boot_test.sh does not take its "nothing to boot" SKIP path.
IMG="$WORK/fake.img"
: > "$IMG"

run_case() {   # run_case <name> <script> <timeout> <expect_rc> <max_seconds>
    local name="$1" script="$2" tmo="$3" want_rc="$4" max_s="$5"
    local t0 t1 rc elapsed
    t0=$(date +%s)
    STUB_SCRIPT="$script" TIMEOUT_S="$tmo" SERIAL_LOG="$WORK/serial.log" \
        EXTRA_SENTINEL="${CASE_EXTRA:-}" FORBIDDEN_SENTINEL="${CASE_FORBIDDEN:-}" \
        bash "$BOOT_TEST" "$IMG" > "$WORK/out.txt" 2>&1
    rc=$?
    t1=$(date +%s); elapsed=$((t1 - t0))
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: $name — exit $rc, expected $want_rc"; sed -n '1,6p' "$WORK/out.txt"; fails=$((fails+1)); return
    fi
    if [ "$elapsed" -gt "$max_s" ]; then
        echo "FAIL: $name — took ${elapsed}s, expected <= ${max_s}s"; fails=$((fails+1)); return
    fi
    echo "PASS: $name (rc=$rc, ${elapsed}s)"
}

echo "=== DDR-785 boot_test.sh self-test ==="

# 1. Eligible for early exit: everything required lands at t=2s under a 60s
#    window. Must PASS *and* finish far short of the window — if the harness
#    waited it out this takes 60s and the <=20s bound fails.
CASE_EXTRA="$(printf 'FS PATTERN OK')" CASE_FORBIDDEN="" \
run_case "early exit when all required sentinels present" \
    "1|NEXUS KERNEL OK;1|FS PATTERN OK" 60 0 20

# 2. THE DANGEROUS CASE. Required sentinel at t=1s, forbidden pattern at t=6s.
#    Early exit would stop before the forbidden line and wrongly PASS. Declaring
#    FORBIDDEN_SENTINEL must disable early exit, so this waits out the 10s window
#    and FAILS (rc=1).
CASE_EXTRA="" CASE_FORBIDDEN="LATE BUG" \
run_case "late forbidden pattern still FAILS (no early exit)" \
    "1|NEXUS KERNEL OK;5|LATE BUG" 10 1 25

# 3. A required pattern that never appears must still FAIL, after the full window.
CASE_EXTRA="$(printf 'NEVER PRINTED')" CASE_FORBIDDEN="" \
run_case "missing required pattern still FAILS" \
    "1|NEXUS KERNEL OK" 8 1 25

# 4. Forbidden declared but never emitted: full window, then PASS.
CASE_EXTRA="" CASE_FORBIDDEN="SOME BUG" \
run_case "forbidden declared but absent still PASSES" \
    "1|NEXUS KERNEL OK" 8 0 25

if [ "$fails" -eq 0 ]; then
    echo "[selftest] PASS — early exit works, and it cannot mask a late forbidden pattern."
    exit 0
fi
echo "[selftest] FAIL — $fails case(s) failed."
exit 1
