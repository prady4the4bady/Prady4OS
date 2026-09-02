#!/usr/bin/env bash
# apt_prepare_selftest.sh — DDR-1045: fixtures for apt_prepare.sh.
#
# This exists because apt_prepare.sh was WRONG TWICE, both times by guessing at
# the runner image instead of checking it (see that script's header). Neither
# guess could be caught locally, so the current version guesses nothing -- and
# the property that replaced the guessing is testable, which is the point.
#
# THE PROPERTY UNDER TEST: a failing `apt-get update` is tolerated, but an
# unusable index is NOT -- the resolve check decides, and it checks exactly the
# packages the caller asked for. apt-get/apt-cache are stubbed on PATH, so no
# root and no real index are involved.
set -uo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
rc=0

# $1 = apt-get update exit code, $2 = "ok" | "none" for apt-cache policy
mkstubs() {
    local d="$tmp/bin"; rm -rf "$d"; mkdir -p "$d"
    cat > "$d/apt-get" <<STUB
#!/usr/bin/env bash
if [ "\$1" = "update" ]; then exit $1; fi
if [ "\$1" = "install" ]; then shift; echo "STUB-INSTALL \$*" >> "$tmp/installed"; exit 0; fi
exit 0
STUB
    cat > "$d/apt-cache" <<STUB
#!/usr/bin/env bash
case "$2" in
  none)  echo "  Candidate: (none)" ;;   # known package, nothing installable
  empty) : ;;                            # UNKNOWN package: real apt prints NOTHING
  *)     echo "  Candidate: 1.0" ;;
esac
STUB
    chmod +x "$d/apt-get" "$d/apt-cache"
    echo "$d"
}

run() { APT_SUDO="" PATH="$1:$PATH" bash tools/ci/apt_prepare.sh "${@:2}"; }

# --- 1: update clean, packages resolve -> install, rc=0 ----------------------
: > "$tmp/installed"; d=$(mkstubs 0 ok)
run "$d" clang nasm >"$tmp/o1" 2>&1
[ $? -ne 0 ] && { echo "FAIL 1: clean update + resolvable packages should succeed"; cat "$tmp/o1"; rc=1; }
grep -q 'STUB-INSTALL .*clang' "$tmp/installed" || { echo "FAIL 1: never installed"; rc=1; }

# --- 2: update FAILS but packages resolve -> must still install, rc=0 --------
#     This is the vendor-repo 403 case, i.e. the whole reason this script exists.
: > "$tmp/installed"; d=$(mkstubs 100 ok)
run "$d" clang nasm >"$tmp/o2" 2>&1
[ $? -ne 0 ] && { echo "FAIL 2: a vendor-repo update failure must NOT fail the job"; cat "$tmp/o2"; rc=1; }
grep -q 'STUB-INSTALL .*nasm' "$tmp/installed" || { echo "FAIL 2: tolerated the failure but never installed"; rc=1; }

# --- 3: update clean but a package has NO candidate -> must REFUSE -----------
#     This is the failure mode attempt 1 shipped: an empty/stale index. Loudly.
: > "$tmp/installed"; d=$(mkstubs 0 none)
run "$d" clang nasm >"$tmp/o3" 2>&1
[ $? -eq 0 ] && { echo "FAIL 3: proceeded with an UNUSABLE index -- attempt 1's bug"; cat "$tmp/o3"; rc=1; }
grep -q 'STUB-INSTALL' "$tmp/installed" && { echo "FAIL 3: installed anyway"; rc=1; }
grep -q 'no candidate for' "$tmp/o3" || { echo "FAIL 3: did not name the cause"; rc=1; }

# --- 5: apt-cache prints NOTHING (real apt's answer for an unknown package) --
#     MEASURED against real apt, and it is not what fixture 3 exercises: an
#     unknown package produces EMPTY output, not "Candidate: (none)". The stub
#     originally only ever emitted the latter, so the branch that handles real
#     apt's actual behaviour was untested. This closes that.
: > "$tmp/installed"; d=$(mkstubs 0 empty)
run "$d" clang nasm >"$tmp/o5" 2>&1
[ $? -eq 0 ] && { echo "FAIL 5: empty apt-cache output treated as resolvable"; cat "$tmp/o5"; rc=1; }
grep -q 'STUB-INSTALL' "$tmp/installed" && { echo "FAIL 5: installed an unknown package"; rc=1; }

# --- 4: no arguments -> usage error, never a silent no-op -------------------
d=$(mkstubs 0 ok)
run "$d" >"$tmp/o4" 2>&1
[ $? -eq 2 ] || { echo "FAIL 4: empty package list should be a usage error"; rc=1; }

[ $rc -eq 0 ] && echo "apt_prepare-selftest OK — 403 tolerated; unusable index refused in BOTH real-apt shapes ((none) and empty); install happens; empty args rejected"
exit $rc
