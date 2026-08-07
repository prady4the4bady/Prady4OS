#!/usr/bin/env bash
# DDR-859 — the guard that makes the CMake/Makefile hybrid safe.
#
# THE FAILURE THIS EXISTS TO CATCH. A hybrid build's real hazard is not two
# build systems; it is TWO SETS OF FLAGS. If CMake ever stops taking its flags
# from the Makefile — someone "simplifies" the query, or print-flags loses a
# variable — CMake still produces a kernel.elf. The gates still pass, because
# they build with the Makefile. And CMake ships a kernel compiled differently
# from the one that was proven. Neither system reports anything.
#
# So this asserts that what CMake captured at configure time is byte-identical
# to what the Makefile reports now.
#
# EXIT CODES, and why 77 exists:
#   0  parity holds
#   1  DRIFT — the two disagree, or the contract is broken
#   77 cmake absent; SKIPPED, not passed
#
# 77 rather than 0 is the whole point. A check that returns success when it
# could not run is indistinguishable from a check that ran and found nothing —
# the defect this project has now hit seventeen times. Same discipline as
# tools/vbox_runner/run_vbox.sh.
set -uo pipefail

cd "$(dirname "$0")/../.."
BUILD_DIR="${CMAKE_PARITY_BUILD_DIR:-build/cmake-parity}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "[cmake-check] SKIPPED (77): cmake is not installed." >&2
  echo "[cmake-check] This is NOT a pass. Install cmake, or run it in the" >&2
  echo "[cmake-check] Docker image (Dockerfile installs it) to verify." >&2
  exit 77
fi

echo "[cmake-check] cmake $(cmake --version | head -1 | awk '{print $3}')"

rm -rf "$BUILD_DIR"
if ! cmake -S . -B "$BUILD_DIR" >/tmp/cmake-parity-configure.log 2>&1; then
  echo "[cmake-check] FAIL: configure failed." >&2
  tail -30 /tmp/cmake-parity-configure.log >&2
  exit 1
fi

CAPTURED="$BUILD_DIR/captured-flags.txt"
if [ ! -f "$CAPTURED" ]; then
  echo "[cmake-check] FAIL: configure produced no captured-flags.txt." >&2
  echo "[cmake-check] CMake is no longer recording what it took from the" >&2
  echo "[cmake-check] Makefile, so drift could not be detected at all." >&2
  exit 1
fi

CURRENT=$(mktemp)
trap 'rm -f "$CURRENT"' EXIT
make --no-print-directory print-flags > "$CURRENT" 2>/dev/null

# Compare ignoring only a trailing blank line; every flag character matters.
if ! diff -q <(sed -e '/^$/d' "$CAPTURED") <(sed -e '/^$/d' "$CURRENT") >/dev/null; then
  echo "[cmake-check] FAIL: CMake's captured flags have DRIFTED from the Makefile." >&2
  echo "[cmake-check] CMake would build a kernel the gates never proved." >&2
  diff <(sed -e '/^$/d' "$CAPTURED") <(sed -e '/^$/d' "$CURRENT") >&2 || true
  exit 1
fi

# The contract itself: every variable CMake depends on must still be reported.
missing=""
for key in KCFLAGS USER_C_CFLAGS KINCLUDES NASM_WERROR X64_TRIPLE \
           KERNEL_LD KERNEL_ELF KERNEL_CS_COUNT; do
  grep -q "^${key}=" "$CURRENT" || missing="$missing $key"
done
if [ -n "$missing" ]; then
  echo "[cmake-check] FAIL: print-flags no longer reports:$missing" >&2
  exit 1
fi

# -Werror is load-bearing (the zero-warnings mandate). Asserted by name rather
# than trusted to the diff, because a diff only fires if CMake's copy is stale —
# if BOTH lost -Werror it would still say parity holds.
for required in "-Werror" "-mcmodel=kernel" "-mno-red-zone"; do
  grep "^KCFLAGS=" "$CURRENT" | grep -q -- "$required" || {
    echo "[cmake-check] FAIL: KCFLAGS no longer carries $required" >&2
    exit 1
  }
done

n_flags=$(grep -c '=' "$CURRENT")
echo "[cmake-check] OK - $n_flags flag variables, CMake matches the Makefile"
exit 0
