#!/usr/bin/env bash
# tools/ci/runner_env.sh — DDR-1048: check an assumption about the CI runner's
# environment BEFORE pushing, instead of by watching real CI fail.
#
# WHY THIS EXISTS. DDR-1045 took three attempts to fix a broken apt step, and two
# of them broke every toolchain-installing job, because a claim about the runner
# image was asserted without any way to check it. The claim was:
#
#     "the Ubuntu archives in /etc/apt/sources.list remain"
#
# That is FALSE on Ubuntu 24.04 (noble), which ships the archives as deb822 in
# /etc/apt/sources.list.d/ubuntu.sources and leaves /etc/apt/sources.list as a
# comment stub SAYING SO. The file that refutes it was on the development box the
# whole time, unread. The gap was never a missing environment -- it was not
# looking at the one already running.
#
# WHAT THIS IS AND IS NOT. This host is Ubuntu 24.04, the same release as
# `ubuntu-latest`, so it answers questions about APT LAYOUT AND APT BEHAVIOUR
# faithfully. It is NOT the runner image: the vendor repos differ (here docker /
# deadsnakes / ondrej; the runner carries packages.microsoft.com), and preinstalled
# tooling differs. So:
#     answerable here  -- where do the archives live? what does apt print for an
#                         unknown package? does removing sources.list.d break
#                         package resolution? does apt-get update exit non-zero
#                         on a vendor 403?
#     NOT answerable   -- which exact vendor repos the runner carries, and what
#                         version of a preinstalled tool it has.
# `report` prints that distinction rather than letting a reader assume parity.
#
# THE ISOLATION THAT MAKES `resolve` MEAN ANYTHING: Dir::State::status is pointed
# at an empty file. Without it, apt-cache policy answers from dpkg's INSTALLED
# set, so a package already present on the box resolves no matter how broken the
# sources are -- the first draft of this tool "passed" for exactly that reason and
# proved nothing.
set -uo pipefail

SP_DEFAULT="${TMPDIR:-/tmp}/runner_env_sbx"
EMPTY_STATUS=""

die() { echo "runner_env: $*" >&2; exit 2; }

_mkempty() {
    EMPTY_STATUS="$(mktemp)"
    : > "$EMPTY_STATUS"
}

# apt options binding every path into the sandbox, plus the dpkg-status isolation.
_aptopts() {
    local sbx="$1"
    printf '%s\n' \
        "-o" "Dir::Etc::sourcelist=$sbx/etc-apt/sources.list" \
        "-o" "Dir::Etc::sourceparts=$sbx/etc-apt/sources.list.d" \
        "-o" "Dir::State::lists=$sbx/lists" \
        "-o" "Dir::Cache=$sbx/cache" \
        "-o" "Dir::State::status=$EMPTY_STATUS"
}

cmd_report() {
    local root="${1:-/}"
    echo "== runner_env report (root=$root) =="
    if [ -r "$root/etc/os-release" ]; then
        # shellcheck disable=SC1090
        echo "os: $(sed -n 's/^PRETTY_NAME="\(.*\)"$/\1/p' "$root/etc/os-release")"
        echo "version_id: $(sed -n 's/^VERSION_ID="\(.*\)"$/\1/p' "$root/etc/os-release")"
    else
        echo "os: UNKNOWN (no $root/etc/os-release)"
    fi

    local sl="$root/etc/apt/sources.list"
    if [ -f "$sl" ]; then
        # NOTE: `grep -c ... || echo 0` is WRONG here -- grep exits 1 on zero
        # matches but has ALREADY printed "0", so the fallback appends a second
        # line and the count becomes "0\n0". Count with awk instead.
        local active
        active="$(awk '!/^[[:space:]]*(#|$)/{n++} END{print n+0}' "$sl" 2>/dev/null)"
        if [ "$active" -eq 0 ]; then
            echo "sources.list: PRESENT but a COMMENT STUB ($active active lines)"
            echo "  -> deleting sources.list.d/* would leave NO archives. This is"
            echo "     precisely the DDR-1045 attempt-1 catastrophe."
        else
            echo "sources.list: $active active line(s)"
        fi
    else
        echo "sources.list: ABSENT"
    fi

    echo "sources.list.d:"
    local f base
    for f in "$root"/etc/apt/sources.list.d/*; do
        [ -e "$f" ] || { echo "  (empty)"; break; }
        base="$(basename "$f")"
        if grep -qE '(archive|security)\.ubuntu\.com|ports\.ubuntu\.com' "$f" 2>/dev/null; then
            echo "  $base   [UBUNTU ARCHIVES]"
        else
            echo "  $base   [vendor/third-party]"
        fi
    done

    echo
    echo "PARITY WITH ubuntu-latest -- what this host can and cannot answer:"
    echo "  CAN : apt source layout, apt's output shapes, whether a sources tree"
    echo "        resolves a package, apt-get update's exit code on a vendor 403."
    echo "  CANNOT: which vendor repos the RUNNER carries (it has"
    echo "        packages.microsoft.com; this host does not), or preinstalled"
    echo "        tool versions. Do not infer those from this report."
}

cmd_sandbox() {
    local sbx="${1:-$SP_DEFAULT}"
    rm -rf "$sbx"
    mkdir -p "$sbx/lists/partial" "$sbx/cache"
    cp -a /etc/apt "$sbx/etc-apt" || die "cannot copy /etc/apt"
    echo "$sbx"
}

cmd_resolve() {
    local sbx="${1:?usage: resolve <sandbox> <pkg>...}"; shift
    [ $# -gt 0 ] || die "usage: resolve <sandbox> <pkg>..."
    _mkempty
    local -a opts; mapfile -t opts < <(_aptopts "$sbx")
    local pkg out rc=0
    for pkg in "$@"; do
        out="$(apt-cache "${opts[@]}" policy "$pkg" 2>/dev/null)"
        if [ -z "$out" ]; then
            # Real apt prints NOTHING for a package it has never heard of. This
            # shape is distinct from "known but uninstallable" and DDR-1045's
            # hand-written stub never emitted it.
            echo "$pkg: UNKNOWN (apt printed nothing)"; rc=1
        elif printf '%s' "$out" | grep -q 'Candidate: (none)'; then
            echo "$pkg: NO CANDIDATE"; rc=1
        else
            echo "$pkg: $(printf '%s' "$out" | sed -n 's/^  Candidate: //p')"
        fi
    done
    rm -f "$EMPTY_STATUS"
    return $rc
}

cmd_update() {
    local sbx="${1:?usage: update <sandbox>}"
    _mkempty
    local -a opts; mapfile -t opts < <(_aptopts "$sbx")
    apt-get "${opts[@]}" update >"$sbx/update.log" 2>&1
    local rc=$?
    echo "apt-get update rc=$rc  403s=$(grep -c '403 Forbidden' "$sbx/update.log")  log=$sbx/update.log"
    rm -f "$EMPTY_STATUS"
    return $rc
}

# The historical mutation: DDR-1045 attempt 1 removed sources.list.d/* believing
# the archives lived in sources.list.
cmd_break_attempt1() {
    local sbx="${1:?usage: break-attempt1 <sandbox>}"
    rm -f "$sbx"/etc-apt/sources.list.d/*
    rm -rf "$sbx/lists"; mkdir -p "$sbx/lists/partial"
    echo "applied DDR-1045 attempt-1 mutation to $sbx"
}

# Anti-vacuity guard: prove the sandbox can still TELL A BROKEN SOURCES TREE FROM
# A GOOD ONE. If arm A and arm B ever agree, this tool has stopped measuring and
# every "verified locally" claim made with it is worthless. Network-free on
# purpose: the host's existing package lists are copied in, so no apt-get update
# is needed and the result cannot depend on a mirror being reachable.
cmd_selftest() {
    local sbx="${TMPDIR:-/tmp}/runner_env_selftest.$$"
    local pkgs=(clang lld llvm nasm xorriso)
    rm -rf "$sbx"; mkdir -p "$sbx/lists/partial" "$sbx/cache"
    cp -a /etc/apt "$sbx/etc-apt"      || die "selftest: cannot copy /etc/apt"
    cp -a /var/lib/apt/lists/. "$sbx/lists/" 2>/dev/null || true

    local a_out b_out a_rc b_rc
    a_out="$(cmd_resolve "$sbx" "${pkgs[@]}")"; a_rc=$?
    cmd_break_attempt1 "$sbx" >/dev/null
    b_out="$(cmd_resolve "$sbx" "${pkgs[@]}")"; b_rc=$?
    rm -rf "$sbx"

    if [ $a_rc -ne 0 ]; then
        echo "runner_env-selftest FAIL: arm A (intact tree) did not resolve:" >&2
        echo "$a_out" >&2; return 1
    fi
    if [ $b_rc -eq 0 ]; then
        echo "runner_env-selftest FAIL: arm B (sources deleted) STILL resolved --" >&2
        echo "  the sandbox is not isolated, so it cannot detect a broken tree." >&2
        echo "$b_out" >&2; return 1
    fi
    if ! printf '%s' "$b_out" | grep -q 'UNKNOWN'; then
        echo "runner_env-selftest FAIL: arm B failed, but not with real apt's" >&2
        echo "  empty-output shape -- the reproduction is not faithful." >&2
        echo "$b_out" >&2; return 1
    fi
    echo "runner_env-selftest OK — intact tree resolves ${#pkgs[@]} pkgs; DDR-1045"\
         "attempt-1 mutation makes every one UNKNOWN (the real CI failure)"
    return 0
}

case "${1:-}" in
    report)          shift; cmd_report "$@" ;;
    sandbox)         shift; cmd_sandbox "$@" ;;
    resolve)         shift; cmd_resolve "$@" ;;
    update)          shift; cmd_update "$@" ;;
    break-attempt1)  shift; cmd_break_attempt1 "$@" ;;
    selftest)        shift; cmd_selftest "$@" ;;
    *) echo "usage: $0 {report [root]|sandbox [dir]|resolve <sbx> <pkg>...|update <sbx>|break-attempt1 <sbx>|selftest}" >&2; exit 2 ;;
esac
