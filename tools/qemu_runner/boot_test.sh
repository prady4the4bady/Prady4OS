#!/usr/bin/env bash
# PRADYOS QEMU smoke test.
# Inert until Phase 1 produces build/pradyos.img. Boots the image headless,
# captures the serial port, and succeeds iff the boot sentinel appears.
set -uo pipefail

# Default sentinel is the kernel's — it prints only after the full chain
# (Stage 1 -> Stage 2 -> A20/E820/CPUID -> protected mode -> long mode -> ring-0
# C) succeeds, so it subsumes the Phase 1 "PRADYOS BOOT OK" gate. Override with
# SENTINEL=... to test an earlier stage.
IMG="${1:-build/pradyos.img}"
SENTINEL="${SENTINEL:-NEXUS KERNEL OK}"
# Optional second pattern that must ALSO appear (e.g. the FAT32 self-test line).
# Empty by default so the plain kernel gate only checks the boot sentinel.
EXTRA_SENTINEL="${EXTRA_SENTINEL:-}"
# Optional patterns (newline-separated) that must NOT appear — e.g. the execve
# "post-exec (BUG)" line that proves execve wrongly returned. Empty by default.
FORBIDDEN_SENTINEL="${FORBIDDEN_SENTINEL:-}"
TIMEOUT_S="${TIMEOUT_S:-30}"
# Serial capture file. Defaults to a private mktemp; override SERIAL_LOG=<path>
# when the host's /tmp is unreliable (e.g. some WSL setups wipe /tmp mid-run,
# corrupting long-timeout gates). CI leaves this unset — the default is used.
SERIAL_LOG="${SERIAL_LOG:-$(mktemp)}"

# DDR-823 / OPEN-9. QEMU's own startup errors go to STDERR, which this harness
# used to leave uncaptured. When QEMU refuses to start — most often because a
# leaked qemu-system-x86_64 from a previous run still holds the image's write
# lock — it exits before emitting a single serial byte. The harness then saw an
# empty capture file and reported "kernel sentinel not found", i.e. it blamed
# the kernel for a host problem.
#
# That misattribution is expensive: it is indistinguishable from a genuine boot
# failure, it reproduces "5/5" for as long as the orphan lives, and it clears on
# its own when the orphan is reaped — which is exactly the OPEN-9 signature that
# twice caused a working change to be blamed and reverted.
QEMU_ERR="$(mktemp)"

if [ ! -f "$IMG" ]; then
    echo "[smoke] no bootable image at '$IMG' yet — expected during Phase 0."
    echo "[smoke] SKIP (nothing to boot)."
    rm -f "$SERIAL_LOG" "$QEMU_ERR"
    exit 0
fi

echo "[smoke] booting $IMG (timeout ${TIMEOUT_S}s, sentinel: '$SENTINEL')..."
# q35 gives a PCIe machine with an ACPI MCFG table (MMCONFIG/ECAM) for Phase 3
# enumeration. Boot from a virtio-blk disk and add a virtio-net device so the
# PCIe scan has real devices to find. -display none + -monitor none keep the
# guest's COM1 the only writer to the capture file (see git history for why
# -nographic corrupts it).
# Optional FAT32 data disk (disk1) for the VFS self-test, and a blank disk
# (disk2) the kernel formats+mounts as SFS — each attached only if present.
FATDISK=()
if [ -f build/fat.img ]; then
    FATDISK=(-drive if=none,format=raw,file=build/fat.img,id=disk1
             -device virtio-blk-pci,drive=disk1)
fi
SFSDISK=()
if [ -f build/sfs.img ]; then
    SFSDISK=(-drive if=none,format=raw,file=build/sfs.img,id=disk2
             -device virtio-blk-pci,drive=disk2)
fi
EXT4DISK=()
# QEMU_NO_EXT4 suppresses the ext4 disk even if build/ext4.img exists — the
# virtio-blk driver caps at VBLK_MAX=4 instances, so a gate that needs its own
# extra disk (smoke-sfs-persist's mkfs image) drops ext4 to stay within the cap.
if [ -f build/ext4.img ] && [ -z "${QEMU_NO_EXT4:-}" ]; then
    EXT4DISK=(-drive if=none,format=raw,file=build/ext4.img,id=disk3
              -device virtio-blk-pci,drive=disk3)
fi

# Optional VirtIO-GPU device (Layer-7 slice 0, ADR-028) — attached only when
# QEMU_GPU is set, so the GPU bring-up gate (smoke-gpu) exercises it while every
# other gate boots without a display device.
GPUDEV=()
if [ -n "${QEMU_GPU:-}" ]; then
    GPUDEV=(-device virtio-gpu-pci)
fi

# Optional SMP (ADR-029) — the smoke-smp gate boots multiple vCPUs; every other
# gate stays -smp 1 (QEMU's default) for single-CPU determinism.
SMPOPT=()
if [ -n "${QEMU_SMP:-}" ]; then
    SMPOPT=(-smp "$QEMU_SMP")
fi

# Optional host-authored SFS disk (DDR-768) — attached LAST (so it lands at the
# highest blk index) only when QEMU_SFS2 is set. The smoke-sfs-persist gate uses
# it to prove the kernel reads a file the host mkfs.sfs wrote; every other gate
# omits it.
SFS2DISK=()
if [ -n "${QEMU_SFS2:-}" ] && [ -f build/mkfs_sfs.img ]; then
    SFS2DISK=(-drive if=none,format=raw,file=build/mkfs_sfs.img,id=sfs2
              -device virtio-blk-pci,drive=sfs2)
fi
# Optional host-provisioned SFS ROOT disk (DDR-770) — a mkfs.sfs image carrying a
# real /etc/aether/config. Attached LAST when QEMU_SFSROOT is set; the kernel
# roots the AETHER daemon there instead of formatting+provisioning blk2.
if [ -n "${QEMU_SFSROOT:-}" ] && [ -f build/sfsroot.img ]; then
    SFS2DISK=(-drive if=none,format=raw,file=build/sfsroot.img,id=sfsroot
              -device virtio-blk-pci,drive=sfsroot)
fi

# Optional NVMe controller (DDR-765) — attached only when QEMU_NVME is set, so
# the smoke-nvme gate exercises controller bring-up + Identify while every other
# gate boots without an NVMe device (the driver is a no-op when none is present).
# DDR-816: virtio-rng, attached only when QEMU_RNG is set. Every other gate
# therefore boots without an entropy device and exercises the fail-closed path.
RNGDEV=()
if [ -n "${QEMU_RNG:-}" ]; then
    RNGDEV=(-object rng-random,filename=/dev/urandom,id=rng0
            -device virtio-rng-pci,rng=rng0)
fi

NVMEDEV=()
if [ -n "${QEMU_NVME:-}" ]; then
    NVMEDEV=(-drive if=none,format=raw,file=build/nvme.img,id=nvm
             -device nvme,serial=PRADYOSNV,drive=nvm)
fi

# DDR-785: exit as soon as every REQUIRED pattern is in the log, instead of always
# burning the whole window. Eligible ONLY when no forbidden patterns are declared:
# a forbidden pattern must NOT appear, and stopping early would merely prove it had
# not appeared YET — a gate that should FAIL could PASS. With only "must appear"
# assertions in play, seeing them sooner cannot change the verdict, so this is
# semantics-preserving by construction rather than by argument. Gates that declare
# FORBIDDEN_SENTINEL keep today's full-window behaviour exactly.
early_exit_eligible=0
[ -z "$FORBIDDEN_SENTINEL" ] && early_exit_eligible=1

# DDR-791 harness gap: every boot runs EVERY in-kernel/user probe, but only the
# ~39 gates that declare FORBIDDEN_SENTINEL watch for a failure string. A probe
# that fails during some OTHER gate's boot therefore prints "... FAIL" into a log
# nobody is checking, and the run goes green. That is how
# "AGENT_METRICS FAIL: agent never observed as scheduled" appeared in run
# 30326561378 without failing it.
#
# These patterns are forbidden in EVERY gate. Deliberately checked against the
# captured log at exit rather than by adding them to FORBIDDEN_SENTINEL: doing
# the latter would make every gate ineligible for DDR-785 early exit and give
# back the measured 46.5 min/run saving.
#
# Known limit, stated rather than glossed: with early exit this catches a probe
# failure that occurred BEFORE the required sentinels appeared — which is the
# observed case — but not one after the run stopped early. A gate that needs the
# later window still declares its own FORBIDDEN_SENTINEL.
# The list is the UNION of every pattern any gate already declares forbidden,
# because every boot runs every probe — if a string means "broken" in one gate it
# means "broken" in all of them.
#
# Two declared patterns are deliberately EXCLUDED because they report state, not
# failure, and promoting them here would turn a condition into an error:
#   PRADYOS_AETHER_CFG_DEFAULT — config fell back to the built-in default, which
#     is correct behaviour in gates that mount no SFS config.
#   msix unavailable          — a device-capability report; only the MSI-X gate
#     is entitled to treat its absence as a failure.
GLOBAL_FORBIDDEN="$(printf '%s\n' \
    'AGENT_METRICS FAIL' 'BIGWRITE FAIL' 'CAPNET FAIL' 'DMESG FAIL' \
    'FSRM FAIL' 'KILL FAIL' 'ROOTMOUNT FAIL' 'SETNAME FAIL' 'SFSROOT FAIL' \
    'SURFDESTROY FAIL' 'SYSINFO FAIL' 'TIME FAIL' \
    'PRIVACYNET FAIL' 'PRADYOS_SOVEREIGN_BYPASSED' \
    'PRADYOS_SIGPIPE_STUB' 'SIGPIPE FAIL' \
    'PRADYOS_SHA256_STUB' 'SHA256 FAIL' \
    'PRADYOS_LOCKBOX_STUB' 'LOCKBOX FAIL' \
    'PRADYOS_RNG_STUB' 'RNG FAIL' \
    'PRADYOS_HKDF_STUB' 'HKDF FAIL' \
    'PRADYOS_FPU_FAIL' 'PRADYOS_NVME_IRQ_FAIL' 'PRADYOS_NVME_PRP_FAIL' \
    'PRADYOS_NVME_RW_FAIL' 'PRADYOS_SFS_NESTED_FAIL' 'PRADYOS_SFS_PERSIST_FAIL' \
    'allowlist FAIL' 'ap preempt FAIL' 'blk integrity FAIL' 'btree churn FAIL' \
    'churn FAIL' 'cross-CPU FAIL' 'current FAIL' 'free-space GC FAIL' \
    'gs FAIL' 'hier dirs FAIL' 'kernel W^X FAIL' 'locks FAIL' \
    'msix on AP FAIL' 'multi-inflight FAIL' 'percpu FAIL' 'resched FAIL' \
    'rqstress FAIL' 'tss FAIL' 'unlink/rmdir FAIL' 'user on AP FAIL' \
    'controller not ready' 'create-iocq failed' 'create-iosq failed' \
    'identify-ctrl failed' 'identify-ns failed' 'reset stuck')"
[ -n "${SKIP_GLOBAL_FORBIDDEN:-}" ] && GLOBAL_FORBIDDEN=""

# Fails the run when any probe reported a failure, whichever gate is running.
check_global_forbidden() {
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if grep -qF "$pat" "$SERIAL_LOG" 2>/dev/null; then
            echo "[smoke] FAIL — a probe reported '$pat' during this gate's boot."
            echo "[smoke] (DDR-791: forbidden in every gate, not only the one that owns it.)"
            echo "[smoke] --- matching lines ---"
            grep -aF "$pat" "$SERIAL_LOG" | head -5

            # DDR-824 / OPEN-10. Printing ONLY the matching lines threw away the
            # diagnosis. Probes are written summary-last: SFS prints
            #   [sfs] churn FAIL op=create iter=17
            # and only then the summary "[sfs] btree churn FAIL" that this list
            # matches on. The op= line — the one naming WHICH operation broke and
            # on which iteration — contains none of the forbidden string, so it
            # was never printed. OPEN-10 was seen twice in CI and remained
            # undiagnosable for exactly that reason.
            #
            # 40 lines of leading context is enough for any probe's own
            # diagnostics without dumping a whole boot.
            echo "[smoke] --- 40 lines of context before the first match (the diagnosis usually lives here) ---"
            grep -aB40 -m1 -F "$pat" "$SERIAL_LOG" | sed 's/^/[smoke]   /'
            return 1
        fi
    done <<EOF
$GLOBAL_FORBIDDEN
EOF
    return 0
}

# True once $SENTINEL and every non-empty EXTRA_SENTINEL line are in the log.
all_required_present() {
    grep -q "$SENTINEL" "$SERIAL_LOG" 2>/dev/null || return 1
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        grep -qF "$pat" "$SERIAL_LOG" 2>/dev/null || return 1
    done <<EOF
$EXTRA_SENTINEL
EOF
    return 0
}

# DDR-804: per-boot probe selection. Empty unless a gate asks for it, and
# the kernel fails closed when the string is absent, so every gate that does
# not set QEMU_PROBES boots exactly as before.
FWCFG=()
if [ -n "${QEMU_PROBES:-}" ]; then
    FWCFG=(-fw_cfg name=opt/org.pradyos/probes,string="${QEMU_PROBES}")
fi

timeout "${TIMEOUT_S}" qemu-system-x86_64 \
    -machine q35 \
    -drive if=none,format=raw,file="$IMG",id=disk0 \
    -device virtio-blk-pci,drive=disk0,bootindex=0 \
    "${FWCFG[@]}" \
    "${FATDISK[@]}" \
    "${SFSDISK[@]}" \
    "${EXT4DISK[@]}" \
    "${SFS2DISK[@]}" \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    "${GPUDEV[@]}" \
    "${SMPOPT[@]}" \
    "${NVMEDEV[@]}" \
    "${RNGDEV[@]}" \
    -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" \
    2>"$QEMU_ERR" &
qemu_pid=$!

if [ "$early_exit_eligible" -eq 1 ]; then
    # Poll the capture file while the guest runs. QEMU writes serial to a FILE,
    # not a pipe, so there is no buffering to defeat. The `timeout` above remains
    # the hard ceiling — this loop only ever stops things EARLIER.
    while kill -0 "$qemu_pid" 2>/dev/null; do
        if all_required_present; then
            kill "$qemu_pid" 2>/dev/null
            break
        fi
        sleep 0.25
    done
fi
wait "$qemu_pid" 2>/dev/null || true

# DDR-823 / OPEN-9 — checked BEFORE every other verdict, including the probe
# check. If QEMU never ran, nothing downstream carries information: the serial
# log is empty for a host reason, and every assertion below would report a
# kernel or probe fault that did not happen.
#
# Distinct exit code 3, not 1. A host-environment failure and a failing gate are
# different events and must not be summed: "5/5 red" means something completely
# different when all five never booted.
if [ -s "$QEMU_ERR" ] && grep -qiE "Failed to get \"?write\"? lock|Is another process using the image" "$QEMU_ERR"; then
    echo "[smoke] HOST-ENV FAIL — STALE QEMU HOLDING IMAGE LOCK."
    echo "[smoke] QEMU refused to start, so this run says NOTHING about the kernel."
    echo "[smoke] A leaked qemu-system-x86_64 from an earlier run still holds"
    echo "[smoke]   $IMG. Fix the host, then re-run:"
    echo "[smoke]     pkill -f qemu-system-x86_64"
    echo "[smoke] Do NOT run two QEMU gates concurrently — they share this image."
    echo "[smoke] --- qemu stderr ---"
    sed 's/^/[smoke]   /' "$QEMU_ERR"
    rm -f "$SERIAL_LOG" "$QEMU_ERR"
    exit 3
fi

# Any other QEMU startup refusal (bad device, missing file, unsupported option)
# is equally not a kernel result. Matched on QEMU's own "qemu-system-x86_64:"
# prefix, which it uses for fatal argument/device errors, and only when the
# serial log is empty — a warning mid-run must not mask a real boot.
if [ -s "$QEMU_ERR" ] && ! [ -s "$SERIAL_LOG" ] && grep -q "^qemu-system-x86_64:" "$QEMU_ERR"; then
    echo "[smoke] HOST-ENV FAIL — QEMU refused to start; no serial output was produced."
    echo "[smoke] This is not a kernel failure. QEMU stderr:"
    sed 's/^/[smoke]   /' "$QEMU_ERR"
    rm -f "$SERIAL_LOG" "$QEMU_ERR"
    exit 3
fi

# Checked FIRST among guest verdicts: a probe failure is a real failure
# regardless of whether this gate's own sentinels happened to appear. Running it
# after the required-pattern checks would let a gate report "PASS" for a boot in
# which something broke.
if ! check_global_forbidden; then
    rm -f "$SERIAL_LOG" "$QEMU_ERR"
    exit 1
fi

if ! grep -q "$SENTINEL" "$SERIAL_LOG"; then
    echo "[smoke] FAIL — kernel sentinel '$SENTINEL' not found. Serial output was:"
    cat "$SERIAL_LOG"
    rm -f "$SERIAL_LOG" "$QEMU_ERR"
    exit 1
fi
# Each non-empty line of EXTRA_SENTINEL is a literal pattern that must also appear.
extra_count=0
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    extra_count=$((extra_count + 1))
    if ! grep -qF "$pat" "$SERIAL_LOG"; then
        echo "[smoke] FAIL — required pattern '$pat' not found. Serial output was:"
        cat "$SERIAL_LOG"
        rm -f "$SERIAL_LOG" "$QEMU_ERR"
        exit 1
    fi
done <<EOF
$EXTRA_SENTINEL
EOF
# Each non-empty line of FORBIDDEN_SENTINEL is a literal pattern that must NOT appear.
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    if grep -qF "$pat" "$SERIAL_LOG"; then
        echo "[smoke] FAIL — forbidden pattern '$pat' appeared. Serial output was:"
        cat "$SERIAL_LOG"
        rm -f "$SERIAL_LOG" "$QEMU_ERR"
        exit 1
    fi
done <<EOF
$FORBIDDEN_SENTINEL
EOF
echo "[smoke] PASS — saw '$SENTINEL'$( [ "$extra_count" -gt 0 ] && echo " + $extra_count FS pattern(s)" )."
rm -f "$SERIAL_LOG" "$QEMU_ERR"
exit 0
