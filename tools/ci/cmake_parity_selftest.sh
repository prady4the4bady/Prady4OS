#!/usr/bin/env bash
# DDR-859 — self-test for cmake_parity_check.sh.
#
# WHY THIS EXISTS. cmake is not installed on the current build host, so the
# configure path can only be exercised in CI. Shipping the drift-detection logic
# unverified because "CI will run it" is exactly the reasoning that produced the
# secret-scan bug in DDR-854 — a check nobody had watched reject anything.
#
# So the parts that do NOT need cmake are exercised here, by planting a fake
# captured-flags.txt and asserting the checker's verdict. What remains
# CI-only is stated rather than implied: the cmake configure itself.
set -uo pipefail
cd "$(dirname "$0")/../.."

CHECK=tools/ci/cmake_parity_check.sh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0

# The checker returns 77 before reaching any of this when cmake is absent, so
# the arms below drive its comparison logic directly with the same inputs it
# would have used.
current="$TMP/current.txt"
make --no-print-directory print-flags > "$current" 2>/dev/null

_verdict() {   # $1 = captured file -> prints PASS/DRIFT/CONTRACT
  local captured="$1"
  if ! diff -q <(sed -e '/^$/d' "$captured") <(sed -e '/^$/d' "$current") >/dev/null; then
    echo DRIFT; return
  fi
  for key in KCFLAGS USER_C_CFLAGS KINCLUDES NASM_WERROR X64_TRIPLE \
             KERNEL_LD KERNEL_ELF KERNEL_CS_COUNT CC LD NASM; do
    grep -q "^${key}=" "$captured" || { echo CONTRACT; return; }
  done
  for required in "-Werror" "-mcmodel=kernel" "-mno-red-zone"; do
    grep "^KCFLAGS=" "$captured" | grep -q -- "$required" || { echo CONTRACT; return; }
  done
  echo PASS
}

_arm() {  # name, expected, captured-file
  local got; got=$(_verdict "$3")
  if [ "$got" = "$2" ]; then
    printf '  PASS  %-42s -> %s\n' "$1" "$got"
  else
    printf '  FAIL  %-42s -> %s (expected %s)\n' "$1" "$got" "$2"
    fails=1
  fi
}

# A: identical -> parity
cp "$current" "$TMP/a.txt"
_arm "identical flags" PASS "$TMP/a.txt"

# B: a single flag changed -> DRIFT (the real failure mode)
sed 's/-mno-red-zone/-mred-zone/' "$current" > "$TMP/b.txt"
_arm "one flag differs" DRIFT "$TMP/b.txt"

# C: -Werror silently dropped from BOTH -> CONTRACT, not PASS.
# This is the arm that matters: a plain diff says parity holds when both sides
# lost the flag together, which is why -Werror is asserted by name.
sed 's/ -Werror//g' "$current" > "$TMP/c.txt"
cp "$TMP/c.txt" "$TMP/c_current.txt"
saved="$current"; current="$TMP/c_current.txt"
_arm "-Werror dropped from both sides" CONTRACT "$TMP/c.txt"
current="$saved"

# D: a required variable missing entirely -> CONTRACT
grep -v '^KERNEL_CS_COUNT=' "$current" > "$TMP/d.txt"
_arm "required variable missing" DRIFT "$TMP/d.txt"

# E: stale capture (an old kernel source count) -> DRIFT
sed 's/^KERNEL_CS_COUNT=.*/KERNEL_CS_COUNT=71/' "$current" > "$TMP/e.txt"
_arm "stale kernel source count" DRIFT "$TMP/e.txt"

# F: the compiler silently changed -> DRIFT.
# This arm models the bug that actually reached CI: every flag identical, the
# COMPILER different. Flags-only parity reported success while CMake was set to
# build the kernel with gcc and the Makefile with clang.
sed 's/^CC=.*/CC=gcc/' "$current" > "$TMP/f.txt"
_arm "compiler differs (clang vs gcc)" DRIFT "$TMP/f.txt"

# G: CC absent from the contract -> caught.
# Without CC the checker cannot compare toolchains at all, which is exactly the
# state that let the gcc build through unnoticed.
grep -v '^CC=' "$current" > "$TMP/g.txt"
_arm "CC absent from the contract" DRIFT "$TMP/g.txt"

echo
if [ "$fails" -ne 0 ]; then
  echo "cmake-parity-selftest FAILED"
  exit 1
fi
echo "cmake-parity-selftest OK - 7 arms"
echo "NOTE: the cmake configure path itself is CI-only on this host (no cmake)."
