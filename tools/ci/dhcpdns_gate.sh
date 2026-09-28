#!/usr/bin/env bash
# DDR-1141 sec.4 -- smoke-dhcpdns: the guest boots on a NON-default slirp
# network, takes its address by DHCP, and resolves names through
# SYS_DNS_RESOLVE against a host responder on 127.0.0.1:53 (slirp's host alias
# 10.77.0.2:53). Arms D/A/L/U/P are guest sentinels checked by boot_test.sh;
# arm H is checked HERE, on the far end: a query the kernel refused must never
# have reached the responder. That is the only arm that can see a kernel which
# audits a refusal and sends the packet anyway.
#
# Port 53 needs root; on a runner that is not root we use sudo -n (DDR-1104
# sec.3's recorded cost, accepted by DDR-1141). The responder is ALWAYS killed
# on the way out.
set -u
IMG="$1"
mkdir -p build/gatelogs
LOG=build/gatelogs/dns_responder.log
rm -f "$LOG"
SUDO=()
[ "$(id -u)" -eq 0 ] || SUDO=(sudo -n)
"${SUDO[@]}" python3 tools/ci/dns_responder.py "$LOG" 53 &
RPID=$!
cleanup() { "${SUDO[@]}" kill "$RPID" 2>/dev/null; wait "$RPID" 2>/dev/null; }
trap cleanup EXIT
for _ in $(seq 1 50); do
    grep -q 'responder up' "$LOG" 2>/dev/null && break
    sleep 0.1
done
if ! grep -q 'responder up' "$LOG" 2>/dev/null; then
    echo "[dhcpdns] FAIL -- host DNS responder did not bind 127.0.0.1:53"
    exit 2
fi

QEMU_NET_USER_OPTS='net=10.77.0.0/24,host=10.77.0.2,dhcpstart=10.77.0.40,dns=10.77.0.3' \
    bash tools/qemu_runner/boot_test.sh "$IMG"
rc=$?
# Arm H is checked on EVERY run, not only after the guest arms pass. It is the
# one arm that sees the far end, and a kernel that leaks a refused name usually
# ALSO misprints a guest sentinel -- so gating H behind boot_test's rc meant M1
# and M2 (DDR-1141 sec.6) could never show the leak H exists to catch.

echo "[dhcpdns] responder log:"; sed 's/^/    /' "$LOG"
fail=0
for want in allowed.pradyos.test released.pradyos.test; do
    grep -qx "$want" "$LOG" || { echo "[dhcpdns] FAIL arm H -- $want never reached the resolver"; fail=1; }
done
for never in denied.pradyos.test private.pradyos.test; do
    grep -qx "$never" "$LOG" && { echo "[dhcpdns] FAIL arm H -- REFUSED query $never reached the resolver (the name leaked)"; fail=1; }
done
[ "$rc" -eq 0 ] || exit "$rc"
[ "$fail" -eq 0 ] || exit 3
echo "[dhcpdns] PASS -- lease, allowlist, privacy, audit, and no refused name left the machine"
