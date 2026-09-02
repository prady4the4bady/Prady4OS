#!/usr/bin/env bash
# apt_prepare_selftest.sh — DDR-1045: fixtures for apt_prepare.sh's classifier.
#
# This exists because the first version of apt_prepare.sh BROKE EVERY CI JOB. It
# deleted /etc/apt/sources.list.d/* on the stated assumption that the Ubuntu
# archives live in /etc/apt/sources.list -- false on Ubuntu 24.04, where noble
# ships them as deb822 at /etc/apt/sources.list.d/ubuntu.sources. No local check
# existed, so the assumption reached CI unverified.
#
# Fixture 1 reproduces the exact noble layout that broke: ubuntu.sources beside
# microsoft-prod.list. It is the regression test for that mistake.
set -uo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
rc=0

mk_noble() {                      # the Ubuntu 24.04 layout, deb822
    mkdir -p "$1"
    cat > "$1/ubuntu.sources" <<'SRC'
Types: deb
URIs: http://azure.archive.ubuntu.com/ubuntu/
Suites: noble noble-updates noble-backports
Components: main restricted universe multiverse
SRC
    cat > "$1/microsoft-prod.list" <<'SRC'
deb [arch=amd64] https://packages.microsoft.com/ubuntu/24.04/prod noble main
SRC
}

# --- 1: noble layout. ubuntu.sources MUST survive; microsoft MUST go. --------
d="$tmp/noble/sources.list.d"; mk_noble "$d"; : > "$tmp/noble/sources.list"
APT_PREPARE_SELFTEST=1 APT_SOURCES_DIR="$d" APT_MAIN_LIST="$tmp/noble/sources.list" \
    APT_RM="rm -f" bash tools/ci/apt_prepare.sh >"$tmp/o1" 2>&1
if [ $? -ne 0 ]; then echo "FAIL 1: exited non-zero on a valid noble layout"; cat "$tmp/o1"; rc=1; fi
[ -f "$d/ubuntu.sources" ]     || { echo "FAIL 1: DELETED ubuntu.sources -- this is the bug that broke CI"; rc=1; }
[ -f "$d/microsoft-prod.list" ] && { echo "FAIL 1: kept the third-party repo that 403'd"; rc=1; }

# --- 2: only third-party sources -> must REFUSE, not update an empty index ---
d2="$tmp/empty/sources.list.d"; mkdir -p "$d2"; : > "$tmp/empty/sources.list"
cat > "$d2/microsoft-prod.list" <<'SRC'
deb [arch=amd64] https://packages.microsoft.com/ubuntu/24.04/prod noble main
SRC
APT_PREPARE_SELFTEST=1 APT_SOURCES_DIR="$d2" APT_MAIN_LIST="$tmp/empty/sources.list" \
    APT_RM="rm -f" bash tools/ci/apt_prepare.sh >"$tmp/o2" 2>&1
[ $? -eq 0 ] && { echo "FAIL 2: proceeded with NO Ubuntu source -- the guard did not fire"; cat "$tmp/o2"; rc=1; }

# --- 3: legacy layout, archives in /etc/apt/sources.list -> must proceed -----
d3="$tmp/legacy/sources.list.d"; mkdir -p "$d3"
echo 'deb http://archive.ubuntu.com/ubuntu jammy main' > "$tmp/legacy/sources.list"
cat > "$d3/azure-cli.list" <<'SRC'
deb [arch=amd64] https://packages.microsoft.com/repos/azure-cli/ noble main
SRC
APT_PREPARE_SELFTEST=1 APT_SOURCES_DIR="$d3" APT_MAIN_LIST="$tmp/legacy/sources.list" \
    APT_RM="rm -f" bash tools/ci/apt_prepare.sh >"$tmp/o3" 2>&1
[ $? -ne 0 ] && { echo "FAIL 3: refused a legacy layout whose archives are in sources.list"; cat "$tmp/o3"; rc=1; }

[ $rc -eq 0 ] && echo "apt_prepare-selftest OK — noble layout preserved, third-party removed, empty-index refused, legacy layout accepted"
exit $rc
