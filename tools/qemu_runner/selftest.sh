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

# DDR-1088: patterns that must appear in the HARNESS OUTPUT for a case, not just
# in its exit code. Cases 1-7 assert a verdict and a duration; case 8 asserts
# that the reporter actually PRINTED the diagnosis, which is a different claim
# and the one that was false.
CASE_EXPECT_OUT=()

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
    local miss=0 want
    if [ "${#CASE_EXPECT_OUT[@]}" -gt 0 ]; then
        for want in "${CASE_EXPECT_OUT[@]}"; do
            grep -qF -- "$want" "$WORK/out.txt" || {
                echo "FAIL: $name — harness output is missing: $want"; miss=1; }
        done
    fi
    if [ "$miss" -ne 0 ]; then fails=$((fails+1)); return; fi
    echo "PASS: $name (rc=$rc, ${elapsed}s)"
}

# DDR-1088 fixture. A capture shaped like the real ones: a full panic report
# (33 lines, banner through `halting.`) EARLY, then 60 filler lines, then an
# `[apfreeze]`. That ordering is the whole point -- `[apfreeze]` is position 2 in
# GLOBAL_FORBIDDEN and `NEXUS KERNEL PANIC` is 71, so the apfreeze becomes the
# FIRST match and takes the -B40 leading-context block, whose window cannot
# reach 60 lines back to the panic. That is CI 34089554836 / 34089556866 exactly.
#
# WHY THE DISTANCE MATTERS, measured before the case was written: put the panic
# NEXT TO the other match and the assertion passes with no fix at all, because
# the existing leading context sweeps it up. An arm that only proves "a panic
# line appeared somewhere in the output" is vacuous here.
panic_report() {
    cat <<'PANIC'
*** NEXUS KERNEL PANIC ***
component: NEXUS isr
exception: #PF page fault  vector=0x000000000000000E  error=0x0000000000000002
RIP=0xFFFFFFFF80026AC5
CS =0x0000000000000008
RFLAGS=0x0000000000000006
RSP=0xFFFFFFFF80142C10
CR2=0x00000000DEADBEEF
RAX=0x000000000000001E
RBX=0x0000000000000000
RCX=0x0000000000181060
RDX=0x0000000006E97400
RSI=0x0000000000000016
RDI=0x00000000E941BA5D
RBP=0xFFFFFFFF80142D90
R8 =0x0000000000000000
R9 =0x0101010101010101
R10=0x0000000000000000
R11=0x0000000000000000
R12=0x0000000000000000
R13=0x0000000000000000
R14=0x0000000000000000
R15=0x0000000000004000
backtrace:
  0xFFFFFFFF80026E3E
  0xFFFFFFFF8003F341
  0xFFFFFFFF8003F1D3
  0xFFFFFFFF8003E6BD
  0xFFFFFFFF80000D1C
  0xFFFFFFFF8000002B
  <frame chain ends: fp=0x0000000000000000>
halting.
PANIC
}

# The same capture as a FILE, for the scan_forbidden.sh arm (case 9).
build_panic_capture() {   # build_panic_capture <path>
    { echo "NEXUS KERNEL OK"
      panic_report
      for i in $(seq 1 60); do echo "[hb] filler line $i"; done
      echo "[apfreeze] cpu=2 ticks=163 rip=0xFFFFFFFF8000C7B7 if=0 pid=22 shot=1"
    } > "$1"
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

# 5. DDR-791 — THE HARNESS GAP. A probe failure from a gate that is NOT this one
#    appears mid-boot, and this gate declares no FORBIDDEN_SENTINEL of its own.
#    Before the global list, every required pattern was present and the gate
#    reported PASS on a boot in which something broke. It must now FAIL.
CASE_EXTRA="" CASE_FORBIDDEN="" \
run_case "foreign probe FAIL fails a gate that never declared it" \
    "1|AGENT_METRICS FAIL: agent never observed as scheduled;1|NEXUS KERNEL OK" \
    10 1 25

# 6. The global list must not fire on a clean boot — otherwise it would turn
#    every gate red and be reverted within the day.
CASE_EXTRA="" CASE_FORBIDDEN="" \
run_case "clean boot is unaffected by the global list" \
    "1|NEXUS KERNEL OK" 10 0 20

# 7. The global check must not cost the DDR-785 saving: a clean gate is still
#    eligible for early exit and must finish far short of its window.
CASE_EXTRA="$(printf 'FS PATTERN OK')" CASE_FORBIDDEN="" \
run_case "global list does not disable early exit" \
    "1|NEXUS KERNEL OK;1|FS PATTERN OK" 60 0 20

# 8. DDR-1088 — THE PANIC REPORT MUST REACH THE OUTPUT. Two independent CI
#    shards on tip e10f494 each NAMED a panic (DDR-1079's fix working) and
#    printed nothing but its banner, because the scan showed matching lines only
#    and aimed its 40 lines of LEADING context at the first pattern in list
#    order. The panic is written summary-FIRST -- 33 lines of exception, vector,
#    registers and backtrace all come AFTER the string being matched.
#
#    TWO ARMS, and the far one carries the claim. `component: NEXUS isr` is one
#    line after the banner, so any trailing window at all satisfies it;
#    `halting.` is thirty-two lines after it and is satisfied only by a window
#    that spans the whole report. Mutant M2 (window shrunk to 5) passes the near
#    arm and fails the far one alone -- it is the version that "prints something
#    after the banner" and would have shipped against a one-arm check.
# The stub replays a ';'-separated script, so the report is fed line by line and
# the filler + apfreeze follow it. Built with a loop rather than inline so the
# ordering is legible and the 60-line distance is visible at the call site.
{
  scr="0|NEXUS KERNEL OK"
  while IFS= read -r l; do scr="$scr;0|$l"; done < <(panic_report)
  for i in $(seq 1 60); do scr="$scr;0|[hb] filler line $i"; done
  scr="$scr;0|[apfreeze] cpu=2 ticks=163 rip=0xFFFFFFFF8000C7B7 if=0 pid=22 shot=1"
  # NOTE: an array assignment cannot be a command prefix in bash, so these are
  # plain statements. run_case reads all three as globals.
  CASE_FORBIDDEN=""
  CASE_EXTRA=""
  CASE_EXPECT_OUT=("component: NEXUS isr" "halting.")
  run_case "panic report reaches the output past a nearer match" "$scr" 10 1 25
}

# 9. The SAME claim for scan_forbidden.sh, which had no context in EITHER
#    direction. It is the scanner smoke-shell and smoke-mce arm F use, so a
#    panic there was one banner line and nothing else. Two copies of one printer
#    drift, so both are asserted.
CASE_EXPECT_OUT=()
build_panic_capture "$WORK/scanfix.log"
if bash "$HERE/scan_forbidden.sh" "$WORK/scanfix.log" sf > "$WORK/sf.txt" 2>&1; then
    echo "FAIL: scan_forbidden.sh reported clean on a capture containing a panic"
    fails=$((fails+1))
elif ! grep -qF -- "halting." "$WORK/sf.txt"; then
    echo "FAIL: scan_forbidden.sh named the panic but did not print its report"
    sed -n '1,8p' "$WORK/sf.txt"; fails=$((fails+1))
else
    echo "PASS: scan_forbidden.sh prints the panic report, not just the banner"
fi

if [ "$fails" -eq 0 ]; then
    echo "[selftest] PASS — early exit works, it cannot mask a late forbidden"
    echo "           pattern, a foreign probe failure now fails every gate, and"
    echo "           a panic's report reaches the output in both scanners."
    exit 0
fi
echo "[selftest] FAIL — $fails case(s) failed."
exit 1
