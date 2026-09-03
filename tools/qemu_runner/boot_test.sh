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
# Serial capture file. §NON-NEGOTIABLE 7: gate logs live under build/gatelogs/,
# NEVER /tmp -- WSL wipes it mid-run and corrupts long-timeout gates. This used
# to default to $(mktemp), i.e. /tmp, while the comment right here admitted /tmp
# was unreliable. That default plus the unconditional delete below is why the
# DDR-985 run-16 panic reached "#PF page fault" and no further: the vector=,
# RIP= and CR2= lines existed and were thrown away. Override SERIAL_LOG=<path>
# to place it elsewhere; CI leaves it unset and gets the repo-local default.
__bt_root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$__bt_root/build/gatelogs"
SERIAL_LOG="${SERIAL_LOG:-$__bt_root/build/gatelogs/serial-$$.log}"
# DDR-988 sec.11.2: TRUNCATE the capture before booting. Until sec.11 every exit
# path deleted it, so a harness that reuses one SERIAL_LOG path got isolation as
# a side effect of the cleanup. Keeping failed captures removed that side effect
# and selftest.sh -- which reuses "$WORK/serial.log" for every case -- began
# matching the PREVIOUS case's forbidden pattern in the window before the stub
# qemu truncates the file. Isolation must not be a side effect of cleanup: a
# gate may never inherit an earlier run's serial content, whatever the path.
: > "$SERIAL_LOG" 2>/dev/null || true
# DDR-1043: the QMP sidecar must be cleared with the capture it belongs to.
# It is not truncated by the line above, so a run reusing this SERIAL_LOG path
# would print the PREVIOUS run's registers as if they were its own — a stale
# artefact presented as current, which is the exact failure mode this whole
# instrument exists to remove.
rm -f "${SERIAL_LOG}.qmpdump" 2>/dev/null || true

# DDR-777 / BUG-1 diagnostics: KEEP_SERIAL=1 preserves the serial capture.
#
# Every exit path below removes the capture, which is correct for a normal gate
# but destroys the only product of a diagnostic run (the [bsp] churn trace).
# Pinning SERIAL_LOG from the environment is NOT enough -- the file is deleted
# regardless of where it lives, which is why three validation attempts produced
# an empty capture and looked like "the instrument is silent".
#
# One helper, used by every removal site, so the opt-in cannot be applied to
# some paths and missed on others. Unset (the default) behaves exactly as before.
serial_rm() {
    [ -n "${KEEP_SERIAL:-}" ] && return 0
    rm -f "$@"
}

# DDR-988 sec.11. The SERIAL_LOG default was moved out of /tmp above so a failing
# gate's capture would survive -- but every failure path still called serial_rm,
# which deletes it whenever KEEP_SERIAL is unset. Relocating the file and then
# deleting it anyway fixes nothing: a failed gate is exactly the run whose
# capture is the only evidence, and CI never sets KEEP_SERIAL, so in CI the
# artefact was destroyed 100% of the time. That is the other half of why the
# DDR-985 run-16 panic stopped at "#PF page fault" with no vector=/RIP=/CR2=.
#
# A failed gate now KEEPS its capture unconditionally and prints the path, so the
# evidence is named in the gate output instead of having to be guessed at. Only
# the PASS path deletes. KEEP_SERIAL still forces retention on a passing run.
serial_keep_fail() {
    for __f in "$@"; do
        [ -e "$__f" ] || continue
        # An EMPTY capture preserves nothing. sec.11 leaked one per SKIP/host-env
        # exit because it only skipped them; it must remove them.
        if [ ! -s "$__f" ]; then rm -f "$__f"; continue; fi
        # Copy to a unique name: the path itself may be reused (and now
        # truncated) by the very next gate, which would destroy the evidence
        # this function exists to preserve.
        __k="${__f}.fail-$$"
        if cp -f "$__f" "$__k" 2>/dev/null; then
            echo "[boot_test] FAIL — capture kept: $__k"
            rm -f "$__f"          # evidence now lives in the .fail copy
        else
            echo "[boot_test] FAIL — capture kept: $__f"   # copy failed: keep it
        fi
        # DDR-989 sec.9.14: PRINT the qemu stderr, do not merely name it.
        #
        # Naming a path is useless in CI: the file lives on a runner that is
        # destroyed with the job, so "capture kept: /home/runner/..." is a
        # dead reference to anyone reading the log afterwards. A smoke-nethammer
        # failure was undiagnosable for exactly this reason -- every sentinel
        # present, no verdict line, and the one file that would have explained
        # it unreachable.
        #
        # This is DDR-979's lesson applied again: that DDR merged make's stderr
        # into the job log (2>&1) precisely so the NEXT panic would be readable
        # instead of guessed at, and it worked -- DDR-996 was root-caused from
        # the first capture after it landed.
        #
        # Only the qemuerr file is dumped. The serial log is already echoed by
        # the harness and is thousands of lines; bounded to the last 40 so a
        # runaway stderr cannot flood the job output.
        case "${__f##*/}" in
            qemuerr*)
                echo "[boot_test] --- qemu stderr (last 40 lines) ---"
                tail -40 "$__k" 2>/dev/null || tail -40 "$__f" 2>/dev/null
                echo "[boot_test] --- end qemu stderr ---"
                ;;
        esac
    done
    return 0
}

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
# DDR-988 sec.11.3: QEMU_ERR was $(mktemp), i.e. /tmp -- the same
# §NON-NEGOTIABLE 7 violation DDR-987 fixed for SERIAL_LOG and missed here. It
# matters more now that sec.11 PRESERVES this file on failure: keeping evidence
# in the directory the rules call unreliable is not keeping it.
QEMU_ERR="$__bt_root/build/gatelogs/qemuerr-$$.log"
: > "$QEMU_ERR" 2>/dev/null || QEMU_ERR="$(mktemp)"

if [ ! -f "$IMG" ]; then
    echo "[smoke] no bootable image at '$IMG' yet — expected during Phase 0."
    echo "[smoke] SKIP (nothing to boot)."
    serial_rm "$SERIAL_LOG" "$QEMU_ERR"   # DDR-988 sec.11.5: SKIP is not a FAIL
    exit 0
fi

# DDR-951 / OPEN-9 stage 2 — PRE-flight stray check.
#
# The post-hoc handler below (search "STALE QEMU HOLDING IMAGE LOCK") already
# stops a stray from being misreported as a kernel failure, but it can only fire
# AFTER this gate has burned its whole timeout window, and only when QEMU
# actually emits the lock message. Two cases slip past it:
#   1. The stray holds a DIFFERENT image. No lock error is printed, so the run
#      proceeds -- but both QEMUs then compete for host CPU, and every timing
#      gate in this tree is a claim about wall-clock. That is a SILENT
#      perturbation, which is worse than a loud failure.
#   2. The stray is mid-shutdown. Whether the lock error appears is then a race,
#      so one host condition yields two different verdicts.
# Checking first collapses both into one deterministic exit 3.
#
# BRACKET FORM IS MANDATORY. `pgrep qemu-system-x86_64` matches NOTHING, ever:
# Linux truncates comm to 15 chars and the name is 18, so every "no stray QEMU"
# observation recorded against the name form was vacuous. The bracket in
# "[q]emu" additionally keeps this command from matching its own pattern.
if command -v pgrep >/dev/null 2>&1; then
    stray="$(pgrep -f "[q]emu-system-x86_64" || true)"
    if [ -n "$stray" ]; then
        stray_flat="$(echo "$stray" | tr '
' ' ')"
        echo "[smoke] HOST-ENV FAIL -- STALE QEMU HOLDS IMAGE LOCK (pre-flight)."
        echo "[smoke] Refusing to start: a qemu-system-x86_64 is already running."
        echo "[smoke] This run was NOT attempted; it says NOTHING about the kernel."
        echo "[smoke] Offending pid(s): $stray_flat"
        echo "[smoke] Inspect before killing -- it may be a gate you started:"
        stray_csv="$(echo "$stray" | paste -sd, -)"
        echo "[smoke]     ps -o pid,etime,args -p $stray_csv"
        serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
        exit 3
    fi
else
    echo "[smoke] NOTE: pgrep unavailable -- pre-flight stray-QEMU check skipped."
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

# DDR-886 (item 22): boot via OVMF instead of the legacy MBR path. The image
# argument is then an ESP (FAT) rather than a disk with a boot sector, so the
# usual -drive/bootindex block is replaced entirely rather than added to.
#
# OVMF_CODE is attached read-only and OVMF_VARS is COPIED per run: the vars
# store is written by the firmware, and sharing one across runs would let one
# gate's boot order leak into the next — an intermittent difference with no
# visible cause.
BOOTDISK=(-drive "if=none,format=raw,file=$IMG,id=disk0"
          -device virtio-blk-pci,drive=disk0,bootindex=0)
UEFIOPT=()
if [ -n "${QEMU_UEFI:-}" ]; then
    OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
    OVMF_VARS_SRC=/usr/share/OVMF/OVMF_VARS_4M.fd
    if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
        echo "[smoke] OVMF not installed ($OVMF_CODE) — cannot test the UEFI path."
        echo "[smoke] FAIL (missing firmware, not a kernel failure)."
        serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
        exit 1
    fi
    OVMF_VARS="$(mktemp)"
    cp "$OVMF_VARS_SRC" "$OVMF_VARS"
    UEFIOPT=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE"
             -drive "if=pflash,format=raw,unit=1,file=$OVMF_VARS")
    # The ESP REPLACES the legacy boot disk rather than joining it. An image with
    # no boot sector sitting on bootindex=0 next to OVMF just falls through to
    # the UEFI shell, which looks exactly like a kernel that never printed.
    BOOTDISK=(-drive "if=none,format=raw,file=$IMG,id=esp"
              -device virtio-blk-pci,drive=esp,bootindex=0)
fi

# DDR-882 (item 17): a genuinely two-node machine for the NUMA gates. Opt-in for
# the same reason as the AHCI and e1000e devices — SRAT changes what the firmware
# publishes to EVERY gate, and a table that appears only sometimes is exactly how
# an intermittent boot difference gets introduced.
#
# 250M/262M, NOT 256M/256M. A 256 MiB boundary is 4 MiB aligned — the largest
# buddy order — so no block can ever straddle it and the re-bucket's
# straddle-splitting path is never exercised. Mutation testing proved that:
# disabling the split still passed. 250 MiB is not 4 MiB aligned, so blocks do
# straddle and the split has to be correct for the gate to pass.
#
# Explicit memory-backend objects rather than the `-numa node,mem=` form: mem= is
# deprecated and QEMU splits the ranges differently across versions, which would
# make the per-node byte assertions depend on the host's QEMU build rather than
# on the parser under test.
#
# `-smp 2` is required for SRAT to carry more than one Local APIC affinity entry
# (QEMU assigns vCPUs to nodes round-robin), so the CPU->node parse that item 37
# depends on is genuinely exercised. The explicit `-numa cpu,socket-id=` form is
# NOT used: it needs a sockets= topology, and the extra vCPU only matters here
# for what SRAT contains, not for how the guest schedules.
#
# Bringing up APs would expose this gate to the unresolved lost-thread defect
# (DDR-880). It does not: `[numa]` prints from numa_init(), immediately after
# acpi_init() and BEFORE lapic_init() starts any AP, so with no FORBIDDEN
# pattern the DDR-785 early exit fires before the SMP proofs ever run. That is
# why this gate declares its rejection check INSIDE the required line
# ("rejected=0") rather than as a FORBIDDEN sentinel — a FORBIDDEN pattern would
# disable early exit and hand the gate the flake.
NUMAOPT=()
if [ -n "${QEMU_NUMA:-}" ]; then
    NUMAOPT=(-m 512M
             -object memory-backend-ram,id=nram0,size=250M
             -object memory-backend-ram,id=nram1,size=262M
             -numa node,nodeid=0,memdev=nram0
             -numa node,nodeid=1,memdev=nram1
             -smp 2)
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
#
# DDR-981 appended '[apfreeze]': the BSP prints that line only after an AP has
# stopped taking its own LAPIC timer interrupt, which was B#3's root cause and,
# per DDR-977 sec.8.2, OPEN-2's block-touching gates. It never appears on a
# healthy boot, so it belongs here rather than in any one gate's
# FORBIDDEN_SENTINEL -- and putting it here keeps every gate's DDR-785 early-exit
# eligibility, which adding it to ~20 recipes would have destroyed.
#
# Its known limit, stated rather than glossed, is the same one recorded above:
# the freeze happens near tick 300 and the dump prints at the first heartbeat a
# full window later (~tick 1000), so a gate that early-exits before then will not
# see it. That gap does not bite where it matters -- every SMP and block gate the
# freeze actually reddens already declares a FORBIDDEN_SENTINEL and therefore
# burns its full window.
# DDR-994 sec.8 -- `[yieldstall]` is deliberately NOT in this list.
#
# It was added here when the detector shipped, on the assumption that any
# 5-second yield-spin is pathological. The first real capture (run 32702146725,
# smoke-vault: `site=mnt_lock spins=68981 ticks=500 pid=45`) shows that
# assumption is not yet established: mnt_lock is held across real block I/O, the
# SFS self-test cycles mount/umount dozens of times, and CI runs under TCG, so a
# long-but-FINITE wait is entirely plausible. 138 spins/tick against a measured
# turnover of ~255/tick fits a deadlock and heavy contention equally well.
#
# Forbidding it therefore reddens gates on a signal that has not been shown to
# be fatal -- inventing a failure rather than detecting one. The detector now
# emits a matching `[yieldstall] RESOLVED` line (sched.c), so a stuck wait is
# distinguishable from a slow one by the ABSENCE of that line. Re-add
# `[yieldstall]` here only once a capture shows an OPENED stall with no RESOLVED
# partner -- that is a hang, and then it belongs in this list.
# DDR-1001: '[ringwalk]' means the wait4 ring walk exceeded its bound, which
# under g_sched_lock is impossible unless the ring is genuinely corrupt. Unlike
# [yieldstall] (removed from this list in DDR-994 because a resolved stall is
# survivable), this one is fatal by construction: the walk is bounded at ~10x
# any observed live-thread count, so exceeding it is never benign.
#
# THIS COMMENT LIVES ABOVE THE ASSIGNMENT, AND MUST STAY THERE. DDR-1001 first
# wrote it BETWEEN `printf '%s\n' \` and the first pattern. A backslash-newline
# splices the lines before the comment is stripped, so `#` then swallowed the
# whole argument list -- and '[ringwalk]' and '[apfreeze]', left without
# continuations, were RUN as commands ("[ringwalk]: command not found", to a
# stderr nobody reads). GLOBAL_FORBIDDEN was the empty string on every gate in
# the repo for four commits. Nothing looked broken: an empty forbidden list
# fails nothing, it just silently stops catching. smoke-selftest case 5 is what
# found it, on all 10 shards at once, which is exactly the job DDR-791 built it
# for. Do not put a comment inside this printf.
# DDR-1010 APPENDED '[percpu] gs FAIL' and '[percpu] current FAIL'. The list
# ALREADY carried 'percpu FAIL' -- which does NOT match either of them, because
# the printed strings are '[percpu] gs FAIL (syscall ctx)' and '[percpu] current
# FAIL (syscall ctx)': there is a word between. 'percpu FAIL' matches only the
# SMP-boot form '[smp] cpu N percpu FAIL'. So the SWAPGS-discipline probe
# (syscall.c:140-147) could announce a broken kernel invariant and only
# smoke-swapgs, which names 'gs FAIL' in its own FORBIDDEN_SENTINEL, would
# notice. Measured (DDR-1010): a smoke-blk-integrity boot printed both, then
# #GP'd in vmm_map_in, froze two APs, and the gate failed three symptoms later
# on 'blk integrity FAIL reference-read'. An entry that looks like it covers a
# family and covers one member of it is worse than no entry, because it reads
# as covered.
# DDR-1009 APPENDED 'NEXUS KERNEL PANIC'. It had ONE emitter (kernel/idt.c:701)
# and ZERO consumers: no gate grepped for it, so a ring-0 panic was caught only
# when it happened to break a gate's own assertion or run out its clock. A panic
# on a boot that had already printed its sentinel PASSED. Same hole DDR-981
# recorded for '[vblk] compl wait timeout'. Verified safe before adding: no gate
# in the tree expects a kernel panic, and the string appears zero times in
# locally-green captures.
GLOBAL_FORBIDDEN="$(printf '%s\n' \
    '[ringwalk]' \
    '[apfreeze]' \
    '[vrinflate]' \
    'AGENT_METRICS FAIL' 'BIGWRITE FAIL' 'CAPNET FAIL' 'DMESG FAIL' \
    'FAT32MC FAIL' 'MODKEYS FAIL' 'NETHAMMER FAIL' \
    'FSRM FAIL' 'KILL FAIL' 'ROOTMOUNT FAIL' 'SETNAME FAIL' 'SFSROOT FAIL' \
    'SURFDESTROY FAIL' 'SYSINFO FAIL' 'TIME FAIL' \
    'PRIVACYNET FAIL' 'PRADYOS_SOVEREIGN_BYPASSED' \
    'PRADYOS_SIGPIPE_STUB' 'SIGPIPE FAIL' \
    'PRADYOS_SHA256_STUB' 'SHA256 FAIL' \
    'PRADYOS_LOCKBOX_STUB' 'LOCKBOX FAIL' \
    'PRADYOS_RNG_STUB' 'RNG FAIL' \
    'PRADYOS_HKDF_STUB' 'HKDF FAIL' \
    'PRADYOS_SHA512_STUB' 'SHA512 FAIL' \
    'PRADYOS_AEAD_STUB' 'AEAD FAIL' \
    'PRADYOS_ED25519_STUB' 'ED25519 FAIL' \
    'PRADYOS_ACC_STUB' 'ACC FAIL' \
    'PRADYOS_FPU_FAIL' 'PRADYOS_NVME_IRQ_FAIL' 'PRADYOS_NVME_PRP_FAIL' \
    'PRADYOS_NVME_RW_FAIL' 'PRADYOS_SFS_NESTED_FAIL' 'PRADYOS_SFS_PERSIST_FAIL' \
    'allowlist FAIL' 'ap preempt FAIL' 'blk integrity FAIL' 'btree churn FAIL' \
    'churn FAIL' 'cross-CPU FAIL' 'current FAIL' 'free-space GC FAIL' \
    'gs FAIL' 'hier dirs FAIL' 'kernel W^X FAIL' 'locks FAIL' \
    'msix on AP FAIL' 'multi-inflight FAIL' 'percpu FAIL' 'resched FAIL' \
    'rqstress FAIL' 'tss FAIL' 'unlink/rmdir FAIL' 'user on AP FAIL' \
    'controller not ready' 'create-iocq failed' 'create-iosq failed' \
    'identify-ctrl failed' 'identify-ns failed' 'reset stuck' \
    'NEXUS KERNEL PANIC' \
    'panic_stage=' \
    '[kline] TRUNC' \
    '[percpu] gs FAIL' '[percpu] current FAIL')"
[ -n "${SKIP_GLOBAL_FORBIDDEN:-}" ] && GLOBAL_FORBIDDEN=""

# Fails the run when any probe reported a failure, whichever gate is running.
check_global_forbidden() {
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if grep -qF "$pat" "$SERIAL_LOG" 2>/dev/null; then
            report_qmpdump
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
# DDR-1043: surface the QMP vCPU dump in the JOB LOG, not only on the runner's
# disk. A sidecar file nobody prints is a sidecar file nobody reads -- the CI
# artefacts that mattered (DDR-1009 §2, DDR-1019) were read out of job logs.
report_qmpdump() {
    [ -s "${SERIAL_LOG}.qmpdump" ] || return 0
    echo "[smoke] --- DDR-1043 QMP vCPU dump (taken ~5 s before the timeout kill) ---"
    cat "${SERIAL_LOG}.qmpdump"
    echo "[smoke] --- end QMP vCPU dump ---"
}

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
    # DDR-842: a comma is QEMU's own option separator, so a multi-probe list
    # like "auditchain,audittamper" truncated the option and QEMU refused to
    # start. QEMU escapes a literal comma by DOUBLING it. The fw_cfg payload
    # the guest receives is unchanged, so probe_enabled() still splits on a
    # single comma. This path had never been exercised: every gate before now
    # passed exactly one probe.
    _probes_escaped="${QEMU_PROBES//,/,,}"
    FWCFG=(-fw_cfg name=opt/org.pradyos/probes,string="${_probes_escaped}")
fi

# DDR-841: machine/CPU are overridable so smoke-chipset can drive the SAME
# runner across x86_64 variants. Defaults are the historical q35 + QEMU's
# default CPU, so every pre-existing gate behaves exactly as before.
QEMU_MACHINE="${QEMU_MACHINE:-q35}"
# DDR-875: attach an ich9-ahci controller with a SATA disk when a gate asks for
# one. Opt-in rather than always-on: an extra controller changes PCI enumeration
# for every gate, and a driver probing a device that is only sometimes present
# is exactly how an intermittent boot difference appears.
# DDR-876: attach an Intel e1000e NIC when a gate asks. Opt-in for the same
# reason as the AHCI device: a second NIC changes PCI enumeration for every
# gate, and the lwIP bridge already owns virtio-net.
E1000DEV=()
if [ -n "${QEMU_E1000E:-}" ]; then
    E1000DEV=(-netdev user,id=net1 -device e1000e,netdev=net1)
fi
AHCIDEV=()
if [ -n "${QEMU_AHCI_IMG:-}" ]; then
    AHCIDEV=(-device ich9-ahci,id=ahci0
             -drive if=none,format=raw,file="${QEMU_AHCI_IMG}",id=satadisk0
             -device ide-hd,drive=satadisk0,bus=ahci0.0)
fi
CPUOPT=()
[ -n "${QEMU_CPU:-}" ] && CPUOPT=(-cpu "${QEMU_CPU}")

# DDR-887: opt-in QMP diagnostic socket. OFF by default, so none of the 106
# gates change behaviour. With QEMU_QMP_DIAG=1 a watcher dumps every vCPU's
# registers ~5 s before the timeout kill — the only way to observe a
# system-wide IRQs-off spinlock deadlock, since nothing in the guest can print
# once every CPU is spinning with IF=0 (kputs itself takes the console lock).
QMPOPT=()
QMP_SOCK=""
if [ -n "${QEMU_QMP_DIAG:-}" ]; then
    QMP_SOCK="$(mktemp -u /tmp/pradyos_qmp.XXXXXX)"
    QMPOPT=(-qmp "unix:${QMP_SOCK},server,nowait")
fi

timeout "${TIMEOUT_S}" qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" \
    "${CPUOPT[@]}" \
    "${BOOTDISK[@]}" \
    "${FWCFG[@]}" \
    "${FATDISK[@]}" \
    "${SFSDISK[@]}" \
    "${EXT4DISK[@]}" \
    "${SFS2DISK[@]}" \
    "${AHCIDEV[@]}" \
    "${E1000DEV[@]}" \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    "${GPUDEV[@]}" \
    "${SMPOPT[@]}" \
    "${NUMAOPT[@]}" \
    "${UEFIOPT[@]}" \
    "${S3OPT[@]}" \
    "${NVMEDEV[@]}" \
    "${RNGDEV[@]}" \
    "${QMPOPT[@]}" \
    -no-reboot -display none -monitor none \
    -serial "file:$SERIAL_LOG" \
    2>"$QEMU_ERR" &
qemu_pid=$!

# DDR-951 -- reap our own child on interruption, not only on the timeout path.
# The normal path kills $qemu_pid when the window expires, so a run that
# completes leaks nothing. An INTERRUPTED run is the actual leak source: this
# project's CI can cancel runs (a human cancelling a job, runner eviction), and
# Ctrl-C does the same locally.
# When a run IS interrupted the shell dies, QEMU is orphaned, and it holds the
# image write lock until someone notices. That orphan is exactly what the
# pre-flight check above trips over on the NEXT run -- so this trap removes the
# CAUSE, while the check above only contains the symptom.
#
# DDR-988 sec.12.4 / DDR-993: this comment twice claimed "CI cancels runs
# ROUTINELY (the workflow concurrency group cancels the older run whenever two
# dispatches land on one ref)". That is FALSE. There is no `concurrency:` block
# anywhere under .github/workflows/ -- grep finds none -- and run 32657350756
# stayed in_progress across two subsequent pushes to the same ref, which a
# concurrency group would have cancelled twice. Cancellation here is occasional
# (a human cancelling a job, runner eviction, local Ctrl-C), not routine.
# The claim was retracted once, in f45f266, but only in the paragraph above:
# the sec.12.2 paragraph below restated it word for word and survived, which is
# how a review caught it still sitting here. A retraction that does not grep for
# its own claim is half a retraction.
#
# DDR-988 sec.12.2: the trap must ALSO preserve the capture. It used to just kill
# and exit 130, bypassing serial_keep_fail entirely — so an interrupted run threw
# away its serial log exactly when that log is most likely to hold the only
# useful diagnosis. Occasional is reason enough: the runs most worth capturing
# are the long ones, which are the likeliest to be cancelled. The original file
# is left in place, but sec.11.2 now truncates SERIAL_LOG at the start of the
# NEXT run, and a cancelled CI workspace is discarded without an artifact
# upload — so "left in place" is not preserved.
on_interrupt() {
    kill "$qemu_pid" 2>/dev/null
    wait "$qemu_pid" 2>/dev/null
    # declared below this trap, so it may legitimately be unset when we fire
    [ -n "${qmp_watcher_pid:-}" ] && kill "$qmp_watcher_pid" 2>/dev/null
    echo "[smoke] INTERRUPTED (SIGINT/SIGTERM) — preserving the capture."
    serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
    exit 130
}
trap on_interrupt INT TERM

# DDR-887 watcher: fire ~5 s before the hard timeout, while QEMU is still alive,
# and append the vCPU dump to the serial capture so it lands in the artifact.
#
# DDR-1043 — ARMED IN CI, AND NARROWED SO ARMING IS CHEAP.
#
# This watcher was fully implemented and NOTHING IN THE REPO EVER SET
# QEMU_QMP_DIAG. So the one instrument that can answer "the kernel printed a
# panic banner and then nothing for 100 s -- what was the CPU doing?" has been
# switched off in CI, which is the only place that failure has ever been seen
# (DDR-1009 §2, DDR-1019, OPEN-1, OPEN-12). Same shape as DDR-986/DDR-1024: a
# diagnostic designed and then not reached, and DDR-1010's rule that an opt-in
# instrument is guaranteed OFF where it matters.
#
# The narrowing is what makes arming it globally free: fire ONLY if the run has
# not yet satisfied its own sentinels. A healthy full-window gate (~39 of them
# declare FORBIDDEN_SENTINEL and so cannot early-exit) is already going to pass,
# and appending a register dump to its log is pure noise. A run still missing a
# sentinel 5 s before the kill is one that is about to fail, and its register
# state is exactly what nobody has ever had. Predicate reuse is deliberate:
# all_required_present() is the same function the early-exit loop consults, so
# the two cannot disagree about what "done" means.
qmp_watcher_pid=""
if [ -n "${QEMU_QMP_DIAG:-}" ]; then
    (
        sleep "$(( TIMEOUT_S > 5 ? TIMEOUT_S - 5 : 1 ))"
        if kill -0 "$qemu_pid" 2>/dev/null && ! all_required_present; then
            # DDR-1043: a SIDECAR, not $SERIAL_LOG. QEMU holds that file open
            # through `-serial file:` and writes at its OWN tracked offset,
            # without O_APPEND. Anything appended here is therefore overwritten
            # by the guest's next serial output -- measured: a dump written into
            # the serial log lost its header and its whole `info cpus` section,
            # and the surviving register text began mid-line ("00000000246").
            # The instrument would have produced a corrupted artefact on the
            # first failure it was ever armed for.
            python3 "$(dirname "$0")/qmp_cpudump.py" "$QMP_SOCK" "${SERIAL_LOG}.qmpdump" \
                >>"${SERIAL_LOG}.qmpdump" 2>&1 || true
        fi
    ) &
    qmp_watcher_pid=$!
fi

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
if [ -n "$qmp_watcher_pid" ]; then
    kill "$qmp_watcher_pid" 2>/dev/null || true
    wait "$qmp_watcher_pid" 2>/dev/null || true
fi
[ -n "$QMP_SOCK" ] && rm -f "$QMP_SOCK"

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
    serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
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
    serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
    exit 3
fi

# Checked FIRST among guest verdicts: a probe failure is a real failure
# regardless of whether this gate's own sentinels happened to appear. Running it
# after the required-pattern checks would let a gate report "PASS" for a boot in
# which something broke.
if ! check_global_forbidden; then
    serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
    exit 1
fi

if ! grep -q "$SENTINEL" "$SERIAL_LOG"; then
    report_qmpdump
    echo "[smoke] FAIL — kernel sentinel '$SENTINEL' not found. Serial output was:"
    cat "$SERIAL_LOG"
    serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
    exit 1
fi
# Each non-empty line of EXTRA_SENTINEL is a literal pattern that must also appear.
extra_count=0
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    extra_count=$((extra_count + 1))
    if ! grep -qF "$pat" "$SERIAL_LOG"; then
        report_qmpdump
        echo "[smoke] FAIL — required pattern '$pat' not found. Serial output was:"
        cat "$SERIAL_LOG"
        serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
        exit 1
    fi
done <<EOF
$EXTRA_SENTINEL
EOF
# Each non-empty line of FORBIDDEN_SENTINEL is a literal pattern that must NOT appear.
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    if grep -qF "$pat" "$SERIAL_LOG"; then
        report_qmpdump
        echo "[smoke] FAIL — forbidden pattern '$pat' appeared. Serial output was:"
        cat "$SERIAL_LOG"
        serial_keep_fail "$SERIAL_LOG" "$QEMU_ERR"
        exit 1
    fi
done <<EOF
$FORBIDDEN_SENTINEL
EOF
echo "[smoke] PASS — saw '$SENTINEL'$( [ "$extra_count" -gt 0 ] && echo " + $extra_count FS pattern(s)" )."
serial_rm "$SERIAL_LOG" "$QEMU_ERR"
exit 0
