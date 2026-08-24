# PRADYOS top-level Makefile — Phase 0 (toolchain + skeleton).
# Real kernel/boot targets arrive in later phases. For now this only proves
# the toolchain is sound and wires the QEMU smoke test.

include tools/build/toolchain.mk

BUILD_DIR := build/toolchain
TC_DIR    := tests/toolchain
RUST_LIB  := $(TC_DIR)/hello_rs/target/$(RUST_TARGET)/release/libhello_rs.a

# Phase 1 — PRADYOS-BOOT (two-stage: MBR loader + protected-mode stage 2)
STAGE1_SRC := boot/mbr/boot.asm
STAGE2_SRC := boot/stage2/stage2.asm
STAGE1_BIN := build/stage1.bin
STAGE2_BIN := build/stage2.bin
IMG        := build/pradyos.img

# Phase 2a — NEXUS kernel (flat binary loaded at 0x10000; see ADR-005)
KERNEL_ASMS := arch/x86_64/boot.asm arch/x86_64/cpu.asm arch/x86_64/isr.asm arch/x86_64/fast_memcpy.asm arch/x86_64/ipc_copy.asm \
               arch/x86_64/context.asm arch/x86_64/syscall_entry.asm \
               arch/x86_64/usermode.asm arch/x86_64/user_image.asm \
               arch/x86_64/ap_boot.asm
# Freestanding ring-3 test programs (Phase 5a): each linked as its own static
# ELF and embedded into the kernel via arch/x86_64/user_image.asm (incbin).
# hello prints from ring 3; wxviol is the W^X negative regression.
USER_SRC     := user/hello.asm
USER_WX_SRC  := user/wxviol.asm
USER_SYS_SRC := user/systest.asm
USER_EXEC_SRC := user/exectest.asm
USER_TLS_SRC := user/tlstest.asm
USER_FPU_SRC := user/fputest.asm
USER_LD      := user/user.ld
USER_ELF     := build/hello.elf
USER_WX_ELF  := build/wxviol.elf
USER_SYS_ELF := build/systest.elf
USER_EXEC_ELF := build/exectest.elf
USER_TLS_ELF := build/tlstest.elf
USER_FPU_ELF := build/fputest.elf

# PROC-D: the minimal musl libc subset (ADR-023). Built by tools/build_musl.sh
# from the pinned third_party/musl submodule + our overlay, into build/ (git-
# ignored). crt1.o is produced as a side-effect of the same script run.
MUSL_DIR  := third_party/musl
MUSL_OVL  := third_party/musl-overlay
MUSL_LIB  := build/musl/lib/libc.a
MUSL_CRT  := build/musl/lib/crt1.o

# PROC-D step 3: the first ring-3 C program, linked against the musl subset.
USER_CMUSL_SRC := user/cmusl.c
USER_CMUSL_ELF := build/cmusl.elf
USER_INIT_SRC  := user/init.c          # 5d: pradyos-init (PID 1), musl C program
USER_INIT_ELF  := build/init.elf
USER_PRISM_SRC := user/prism.c         # 5e: PRISM shell — placed on FAT32, init execve's it
USER_PRISM_ELF := build/prism.elf
USER_AETHERD_SRC := user/aether_daemon.c  # L6: AETHER daemon (PID-2, CAP_SOVEREIGN)
USER_AETHERD_ELF := build/aether_daemon.elf
USER_AGENT_SRC   := user/agent_base.c     # L6: AETHER agent template (CAP_AGENT)
USER_AGENT_ELF   := build/agent_base.elf
USER_AGENT_DEFS  ?=                       # extra -D for the agent (live mode sets AETHER_TEST_MODE=0)
USER_INPUT_SRC   := user/inputtest.c      # L7: ring-3 keyboard input reader (DDR-703)
USER_INPUT_ELF   := build/inputtest.elf
USER_COMP_SRC    := user/compositor.c     # L7: in-house sovereign-desktop compositor (DDR-704)
USER_COMP_ELF    := build/compositor.elf
USER_SURF_SRC    := user/surfacetest.c    # L7: per-client surface test window (DDR-706)
USER_SURF_ELF    := build/surfacetest.elf
USER_SURFDESTROY_SRC := user/surfdestroytest.c  # L7: surface lifecycle/destroy test (DDR-729)
USER_SURFDESTROY_ELF := build/surfdestroytest.elf
USER_AGENTMETRICS_SRC := user/agentmetricstest.c  # L7: per-agent live metrics probe (DDR-730)
USER_AGENTMETRICS_ELF := build/agentmetricstest.elf
USER_CAPNET_SRC := user/capnettest.c      # L6/7: CAP_NET socket-authority probe (DDR-731)
USER_CAPNET_ELF := build/capnettest.elf
USER_ROOTMNT_SRC := user/rootmounttest.c  # fs: per-process root-mount probe (DDR-739)
USER_ROOTMNT_ELF := build/rootmounttest.elf
USER_FSRM_SRC := user/fsrmtest.c          # fs: ring-3 file lifecycle probe (DDR-744)
USER_FSRM_ELF := build/fsrmtest.elf
USER_FAT32MC_SRC := user/fat32mctest.c    # DDR-973: FAT32 multi-cluster read regression probe
USER_FAT32MC_ELF := build/fat32mctest.elf
USER_NETHAMMER_SRC := user/nethammer.c    # DDR-990: two-CPU connect/close hammer
USER_NETHAMMER_ELF := build/nethammer.elf
USER_MODKEYS_SRC := user/modkeystest.c     # DDR-991: modifier / extended-key probe
USER_MODKEYS_ELF := build/modkeystest.elf
USER_STACKD_SRC := user/stackdemand.c     # ADR-038: demand-paged stack probe
USER_STACKD_ELF := build/stackdemand.elf
USER_FTRUNC_SRC := user/ftrunctest.c      # fs: ring-3 ftruncate probe (DDR-866)
USER_FTRUNC_ELF := build/ftrunctest.elf
USER_RENAME_SRC := user/renametest.c      # fs: SYS_RENAME on SFS (DDR-956/962)
USER_RENAME_ELF := build/renametest.elf
USER_BENCH_SRC  := user/benchtest.c       # perf: RDTSC path benchmark (DDR-870)
USER_BENCH_ELF  := build/benchtest.elf
USER_SYSINFO_SRC := user/sysinfotest.c    # sys: SYS_SYSINFO introspection probe (DDR-748)
USER_SYSINFO_ELF := build/sysinfotest.elf
USER_TIME_SRC := user/timetest.c          # sys: SYS_TIME wall-clock probe (DDR-749)
USER_TIME_ELF := build/timetest.elf
USER_DMESG_SRC := user/dmesgtest.c        # sys: SYS_DMESG kernel-log probe (DDR-750)
USER_DMESG_ELF := build/dmesgtest.elf
USER_KILL_SRC := user/killtest.c          # proc: SYS_KILL fork/kill/reap probe (DDR-755)
USER_KILL_ELF := build/killtest.elf
USER_SETNAME_SRC := user/setnametest.c    # proc: SYS_SETNAME self-rename probe (DDR-756)
USER_SETNAME_ELF := build/setnametest.elf
USER_FUZZ_SRC := user/syscallfuzz.c       # sec: hostile-syscall fuzz probe (DDR-758)
USER_FUZZ_ELF := build/syscallfuzz.elf
USER_EGAUD_SRC := user/egressaudittest.c  # DDR-801: per-destination egress audit
USER_EGAUD_ELF := build/egressaudittest.elf
USER_HKDF_SRC := user/hkdftest.c         # DDR-818: HKDF RFC 5869 vectors
USER_HKDF_ELF := build/hkdftest.elf
USER_X25519_SRC := user/x25519test.c     # DDR-820: X25519 RFC 7748 vectors
USER_X25519_ELF := build/x25519test.elf
USER_SHA512_SRC := user/sha512test.c     # DDR-821: SHA-512 FIPS 180-4 vectors
USER_SHA512_ELF := build/sha512test.elf
USER_AEAD_SRC := user/aeadtest.c         # DDR-819: ChaCha20-Poly1305 RFC 8439 vectors
USER_AEAD_ELF := build/aeadtest.elf
USER_ED25519_SRC := user/ed25519test.c   # DDR-821: Ed25519 RFC 8032 vectors
USER_ED25519_ELF := build/ed25519test.elf
USER_ACC_SRC := user/acctest.c           # DDR-813: ACC envelope gate
USER_ACC_ELF := build/acctest.elf
USER_AGS_SRC := user/agstest.c           # DDR-814: AGS goal-signing gate
USER_AGS_ELF := build/agstest.elf
USER_ACCROT_SRC := user/accrottest.c     # DDR-815: ACC rotation gate
USER_ACCROT_ELF := build/accrottest.elf
USER_VAULT_SRC := user/vaulttest.c       # DDR-834: credential vault gate
USER_VAULT_ELF := build/vaulttest.elf
USER_AMEM_SRC := user/agentmemtest.c     # DDR-836: agent memory gate
USER_AMEM_ELF := build/agentmemtest.elf
USER_CKPT_SRC := user/ckpttest.c         # DDR-837: checkpoint/resume gate
USER_CKPT_ELF := build/ckpttest.elf
USER_SDEP_SRC := user/spawndepthtest.c   # DDR-838: spawn-depth cap gate
USER_SDEP_ELF := build/spawndepthtest.elf
USER_DAG_SRC := user/actiondagtest.c     # DDR-839: DAG action queue gate
USER_DAG_ELF := build/actiondagtest.elf
USER_CRW_SRC := user/coderewritetest.c   # DDR-842: code-rewrite approval gate
USER_CRW_ELF := build/coderewritetest.elf
USER_ACH_SRC := user/auditchaintest.c    # DDR-842: audit chain gate
USER_ACH_ELF := build/auditchaintest.elf
USER_INV_SRC := user/invarianttest.c     # DDR-844: S1-S8 attack gate
USER_INV_ELF := build/invarianttest.elf
USER_LOCKBOX_SRC := user/lockboxtest.c   # DDR-812: metric lockbox read/verify
USER_LOCKBOX_ELF := build/lockboxtest.elf
USER_SHA256_SRC := user/sha256test.c     # DDR-811: SHA-256 NIST vector probe
USER_SHA256_ELF := build/sha256test.elf
USER_SIGPIPE_SRC := user/sigpipetest.c    # DDR-805: SIGPIPE probe (DDR-804 opt-in)
USER_SIGPIPE_ELF := build/sigpipetest.elf
USER_PRIVNET_SRC := user/privacynettest.c # DDR-802: privacy netfilter probe (DDR-804 opt-in)
USER_PRIVNET_ELF := build/privacynettest.elf
USER_SOVEG_SRC := user/sovegresstest.c    # DDR-800: sovereign-egress audit probe
USER_SOVEG_ELF := build/sovegresstest.elf
USER_RTCMONO_SRC := user/rtcmonotest.c    # DDR-796: SYS_CLOCK monotonicity under SMP
USER_RTCMONO_ELF := build/rtcmonotest.elf
USER_METRIC_SRC := user/metrictest.c      # F#68/DDR-795: sealed metric-region probe
USER_METRIC_ELF := build/metrictest.elf
USER_SFSROOT_SRC := user/sfsroottest.c    # fs: persistent SFS-root probe (DDR-760)
USER_SFSROOT_ELF := build/sfsroottest.elf
USER_BIGWRITE_SRC := user/bigwritetest.c  # fs: ring-3 large-write probe (DDR-764)
USER_BIGWRITE_ELF := build/bigwritetest.elf
USER_C_LD      := user/user_c.ld
# user-C compile flags: -mcmodel=large (the 0x8000000000 base exceeds 32-bit
# relocs), musl public headers + our generated bits/. OUR code -> -Werror.
USER_C_CFLAGS  := --target=x86_64-elf -ffreestanding -fno-pic -fno-pie -mcmodel=large \
  -nostdinc -nostdlib -Wall -Wextra -Werror \
  -Iuser/include -Ibuild/musl/include -I$(MUSL_DIR)/arch/x86_64 -I$(MUSL_DIR)/arch/generic -I$(MUSL_DIR)/include
KERNEL_CS   := kernel/main.c kernel/console.c kernel/idt.c kernel/irq.c \
               kernel/mm/pmm.c kernel/mm/numa.c kernel/mm/kheap.c kernel/mm/vmm.c kernel/mm/vmm_cow.c kernel/mm/uaccess.c kernel/cap.c \
               kernel/proc/sched.c kernel/proc/tss.c kernel/proc/fd.c kernel/proc/pipe.c kernel/proc/epoll.c kernel/proc/signal.c kernel/ipc/ipc.c \
               kernel/ipc/bcast.c kernel/syscall/syscall.c kernel/syscall/sys_io.c kernel/syscall/sys_file.c kernel/syscall/sys_proc.c kernel/syscall/sys_mmap.c kernel/syscall/sys_exec.c kernel/syscall/sys_fork.c kernel/syscall/sys_wait.c kernel/syscall/sys_io_uring.c kernel/acpi/acpi.c \
               kernel/drivers/pcie/pcie.c kernel/drivers/virtio/virtio_ring.c \
               kernel/drivers/virtio/virtio.c kernel/drivers/virtio/virtio_pci.c \
               kernel/drivers/blk/blk.c kernel/drivers/blk/virtio_blk.c kernel/drivers/blk/ramdisk.c \
               kernel/drivers/net/virtio_net.c kernel/drivers/net/e1000e.c kernel/drivers/net/netbuf.c \
               kernel/drivers/gpu/virtio_gpu.c \
               kernel/drivers/nvme/nvme.c kernel/drivers/ahci/ahci.c \
               kernel/drivers/input/ps2kbd.c kernel/drivers/input/virtio_input.c \
               kernel/drivers/rtc/rtc.c \
               kernel/drivers/fwcfg/fwcfg.c \
               kernel/crypto/sha256.c kernel/crypto/acc.c kernel/crypto/x25519.c kernel/crypto/fe25519.c kernel/crypto/hkdf.c kernel/crypto/aead.c kernel/crypto/ed25519.c kernel/crypto/sha512.c \
               kernel/drivers/rng/virtio_rng.c \
               kernel/fs/vfs/vfs.c kernel/fs/fat32/fat32.c kernel/fs/sfs/sfs.c kernel/fs/pdrive/pdrive.c kernel/arch/x86_64/pstate.c \
               kernel/fs/sfs/lz4.c kernel/fs/ext4/ext4.c kernel/exec/elf.c kernel/string.c \
               kernel/arch/x86_64/cpu_mitigations.c kernel/vdso/vdso_page.c \
               kernel/aether/aether.c kernel/aether/aether_queue.c kernel/aether/aether_audit.c kernel/aether/aether_mem.c kernel/syscall/sys_aether.c kernel/syscall/sys_socket.c kernel/syscall/sys_acc.c kernel/syscall/sys_fb.c kernel/syscall/sys_surface.c \
               kernel/apic/lapic.c kernel/apic/ioapic.c kernel/apic/smp.c kernel/apic/percpu.c
KERNEL_LD   := kernel/kernel.ld
KERNEL_ELF  := build/kernel.elf
KERNEL_BIN  := build/kernel.bin
# boot.o MUST be first so kernel_entry (.text.boot) lands at the image start.
KERNEL_OBJS := build/boot.o build/cpu.o build/isr.o build/context.o \
               build/syscall_entry.o build/usermode.o build/main.o \
               build/console.o build/idt.o build/irq.o build/pmm.o build/numa.o build/kheap.o \
               build/vmm.o build/vmm_cow.o build/uaccess.o build/cap.o build/sched.o build/tss.o build/fd.o build/pipe.o build/epoll.o build/signal.o build/ipc.o \
               build/bcast.o build/syscall.o build/sys_io.o build/sys_file.o build/sys_proc.o build/sys_mmap.o build/sys_exec.o build/sys_fork.o build/sys_wait.o build/sys_io_uring.o build/acpi.o build/pcie.o \
               build/virtio_ring.o build/virtio.o build/virtio_pci.o build/blk.o \
               build/virtio_blk.o build/ramdisk.o build/virtio_net.o build/e1000e.o build/netbuf.o build/virtio_gpu.o build/nvme.o build/ahci.o build/rtc.o build/fwcfg.o build/sha256.o build/sha512.o build/fe25519.o build/x25519.o build/hkdf.o build/aead.o build/ed25519.o build/acc.o build/sys_acc.o build/ags.o build/sys_ags.o build/vault.o build/sys_vault.o build/agentmem.o build/sys_agentmem.o build/sys_checkpoint.o build/sys_rewrite.o build/sys_audit.o build/virtio_rng.o build/vfs.o build/fat32.o build/sfs.o build/pdrive.o build/pstate.o build/lz4.o \
               build/ext4.o build/elf.o build/user_image.o build/string.o build/fast_memcpy.o build/ipc_copy.o build/cpu_mitigations.o build/vdso_page.o build/metric_page.o \
               build/aether.o build/aether_queue.o build/aether_audit.o build/aether_mem.o build/sys_aether.o build/sys_socket.o build/sys_fb.o build/sys_input.o build/ps2kbd.o build/virtio_input.o build/sys_surface.o \
               build/lwip_port.o build/lapic.o build/ioapic.o build/smp.o build/percpu.o build/ap_boot.o
# Kernel include search paths (so "#include "pmm.h"" resolves after the
# kernel/ subdirectory reorganization).
KINCLUDES   := -Ikernel -Ikernel/mm -Ikernel/proc -Ikernel/ipc -Ikernel/syscall \
               -Ikernel/acpi -Ikernel/drivers/pcie -Ikernel/drivers/virtio -Ikernel/drivers/rtc -Ikernel/drivers/fwcfg -Ikernel/crypto -Ikernel/drivers/rng \
               -Ikernel/drivers/blk -Ikernel/drivers/net -Ikernel/drivers/gpu -Ikernel/drivers/nvme -Ikernel/drivers/ahci -Ikernel/drivers/input -Ikernel/fs/vfs -Ikernel/fs/fat32 -Ikernel/fs/sfs -Ikernel/fs/pdrive \
               -Ikernel/fs/ext4 -Ikernel/exec -Ikernel/include -Ikernel/arch/x86_64 -Ikernel/vdso -Ikernel/aether -Ikernel/apic
KCFLAGS     := --target=$(X64_TRIPLE) -ffreestanding -fno-pic -fno-pie \
               -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
               -fno-stack-protector -fno-omit-frame-pointer \
               -nostdlib -Wall -Wextra -Werror $(KINCLUDES)
# KASAN-style hardening (IMP-B): default ON. Poisons every freed PMM frame and
# arms an 8-byte slab canary checked on each kmalloc, so all gates double as
# poison/use-after-free tests. Build with KASAN=0 to disable for A/B comparison.
KASAN ?= 1
ifeq ($(KASAN),1)
KCFLAGS += -DKASAN=1
endif
# DDR-790: per-pipe create/destroy tracing, OFF by default. It is high-volume and
# smoke-dmesg reads back only the last 4 KiB of the log ring, so an unconditional
# trace evicts that gate's marker (run 30303017178). Enable with PIPE_TRACE=1 when
# chasing the double-free panic.
PIPE_TRACE ?= 0
KCFLAGS += -DPIPE_TRACE=$(PIPE_TRACE)
# DDR-777/DDR-791 (BUG-1): per-iteration BSP-liveness markers in the kmain SFS
# churn block. Same reasoning as PIPE_TRACE — high volume, so OFF by default and
# built only by `make smoke-rqstress-liveness`, which restores a clean image.
BSP_LIVENESS ?= 0
KCFLAGS += -DBSP_LIVENESS=$(BSP_LIVENESS)
# Treat every assembler warning as fatal too (user mandate: zero warnings).
NASM_WERROR := -Werror

.PHONY: smoke-blk-timeout smoke-fs-liveness all setup toolchain-check kernel musl lwip image smoke smoke-selftest smoke-fpu smoke-init smoke-shell smoke-fs smoke-fs-rw smoke-fs-sfs-rw smoke-fs-ext4 smoke-user smoke-uaccess smoke-sysio smoke-sysfile smoke-sysproc smoke-sysmmap smoke-sysexec smoke-sysfork smoke-syswait smoke-mitigations smoke-pmm-poison smoke-vdso smoke-cowfork smoke-net smoke-net-lo smoke-net-fuzz smoke-aether smoke-aether-queue smoke-aether-sec smoke-agent-live smoke-mode smoke-gpu smoke-fs-budget smoke-nvme smoke-mkfs-sfs smoke-sfs-persist smoke-aether-sfsroot smoke-fb smoke-input smoke-compositor smoke-mouse smoke-surface smoke-agents smoke-focus smoke-ambiance smoke-drag smoke-syspipe smoke-sysepoll smoke-syssignal smoke-sysiouring smoke-rqstress-liveness smoke-metric smoke-rtc-smp smoke-serialflood smoke-sovereign-egress smoke-egress-audit smoke-x25519 smoke-sfs-btree-smp4 smoke-sha512 smoke-aead smoke-ed25519 smoke-acc smoke-ftruncate smoke-rename smoke-rename-sfs smoke-bench smoke-ahci smoke-e1000e smoke-numa smoke-numa-alloc smoke-uefi esp-image iso smoke-iso-x86 smoke-iso-userspace smoke-fat32-multicluster ahci-image fat-image sfs-image ext4-image clean ci-shard-check ci-start-align-check ci-probe-rodata-check

# ---------------------------------------------------------------------------
# DDR-859 - print-flags: the Makefile is the SINGLE SOURCE OF TRUTH for build
# flags, and CMake reads them from here rather than restating them.
#
# The hazard a hybrid build actually has is not "two build systems"; it is TWO
# SETS OF FLAGS. If CMake restated -mcmodel=kernel or dropped -Werror it would
# still produce a kernel.elf, the gates would keep passing against the
# Makefile's binary, and CMake would ship a different one. Nothing would report
# it.
#
# So there is exactly one definition, here, and `cmake-check` asserts that what
# CMake captured still matches. Machine-readable on purpose: a human-formatted
# dump invites a parser that "mostly" works.
.PHONY: print-flags print-kernel-sources cmake-check
print-flags:
	@printf 'KCFLAGS=%s\n' '$(KCFLAGS)'
	@printf 'USER_C_CFLAGS=%s\n' '$(USER_C_CFLAGS)'
	@printf 'KINCLUDES=%s\n' '$(KINCLUDES)'
	@printf 'NASM_WERROR=%s\n' '$(NASM_WERROR)'
	@printf 'X64_TRIPLE=%s\n' '$(X64_TRIPLE)'
	@printf 'KERNEL_LD=%s\n' '$(KERNEL_LD)'
	@printf 'KERNEL_ELF=%s\n' '$(KERNEL_ELF)'
	@printf 'KERNEL_CS_COUNT=%s\n' '$(words $(KERNEL_CS))'
	@printf 'CC=%s\n' '$(CC)'
	@printf 'LD=%s\n' '$(LD)'
	@printf 'NASM=%s\n' '$(NASM)'

print-kernel-sources:
	@printf '%s' '$(KERNEL_CS)'

# DDR-859. Fails if CMake's captured flags have drifted from the Makefile's.
# Exits 77 (not 0) when cmake is absent, so "not installed" can never be
# mistaken for "verified" - the same discipline as tools/vbox_runner.
cmake-check:
	@bash tools/ci/cmake_parity_check.sh

# DDR-817. Host-only, no QEMU: every smoke-* gate is in exactly one CI shard, or
# is excluded WITH a stated reason. Guards the failure mode a sharded suite
# actually has — a gate in no shard, so CI is faster because it silently stopped
# running something. Before DDR-817 that was already true of eight gates.
ci-probe-rodata-check:
	bash tools/ci/probe_rodata_check.sh

ci-start-align-check:
	bash tools/ci/start_align_check.sh

ci-shard-check:
	bash tools/ci/shard_check.sh

# Operator directive 2026-08-23 §6.3 — risk-tiered fast lane.
#
#   make smoke-fast GATE=smoke-wmmax           # COUNT defaults from the tier
#   make smoke-fast GATE=smoke-smp COUNT=20    # explicit override
#
# The default COUNT comes from the gate's risk tier in tools/ci/gate_shards.txt
# (4th column): `fast` -> 5, `strict` -> 20. A gate that is not in the manifest
# defaults to 20, and so does anything the allow-list did not explicitly mark
# visual — the tier annotation fails safe, so this target can never quietly
# weaken a scheduler/SMP/security campaign to N=5.
#
# Runs through tools/ci/campaign.sh, so the run survives its invoking shell and
# reports progress in build/campaign_status.txt (§6.1).
.PHONY: smoke-fast
smoke-fast:
	@test -n "$(GATE)" || { echo "usage: make smoke-fast GATE=<smoke-target> [COUNT=<n>]"; exit 1; }
	@tier=$$(awk -F'\t' -v g="$(GATE)" '$$2==g {print $$4}' tools/ci/gate_shards.txt); \
	 if [ -z "$$tier" ]; then tier=strict; echo "[smoke-fast] $(GATE) not in the manifest -> assuming strict"; fi; \
	 n="$(COUNT)"; \
	 if [ -z "$$n" ]; then if [ "$$tier" = fast ]; then n=5; else n=20; fi; fi; \
	 echo "[smoke-fast] $(GATE) tier=$$tier count=$$n"; \
	 bash tools/ci/campaign.sh start "$(GATE)" "$$n" && \
	 bash tools/ci/campaign.sh wait

all:
	@echo "PRADYOS — Phase 2a (NEXUS kernel entry: boot -> long mode -> ring 0 C)."
	@echo "  make setup            # install the toolchain (WSL2/Ubuntu)"
	@echo "  make toolchain-check  # prove clang + nasm + rust + lld interoperate"
	@echo "  make kernel           # build the NEXUS kernel -> $(KERNEL_BIN)"
	@echo "  make image            # stage1 + stage2 + kernel -> $(IMG)"
	@echo "  make smoke            # build image + QEMU boot test (greps sentinel)"

setup:
	bash tools/build/setup_toolchain.sh

# Compile one C unit, one NASM unit, and one no_std Rust staticlib, then link
# them together with ld.lld. Exit 0 == the toolchain is sound.
toolchain-check:
	@mkdir -p $(BUILD_DIR)
	$(CC) --target=$(X64_TRIPLE) -ffreestanding -fno-stack-protector -mno-red-zone \
	      -nostdlib -Wall -Wextra -c $(TC_DIR)/hello.c   -o $(BUILD_DIR)/hello_c.o
	$(CC) --target=$(X64_TRIPLE) -ffreestanding -fno-stack-protector -mno-red-zone \
	      -nostdlib -Wall -Wextra -c $(TC_DIR)/tc_main.c -o $(BUILD_DIR)/tc_main.o
	$(NASM) -f elf64 $(TC_DIR)/hello.asm -o $(BUILD_DIR)/hello_asm.o
	cd $(TC_DIR)/hello_rs && $(CARGO) build --release --target $(RUST_TARGET)
	$(LD) -nostdlib -e _start -o $(BUILD_DIR)/toolchain_check.elf \
	      $(BUILD_DIR)/tc_main.o $(BUILD_DIR)/hello_c.o $(BUILD_DIR)/hello_asm.o $(RUST_LIB)
	@echo "OK: clang + nasm + rust(no_std) + ld.lld linked -> $(BUILD_DIR)/toolchain_check.elf"

# PROC-D: build the musl subset (libc.a + crt1.o) from the pinned submodule.
# crt1.o is a by-product of the same script run (empty recipe, ordered after lib).
$(MUSL_LIB): tools/build_musl.sh $(MUSL_OVL)/syscall_overrides.h $(MUSL_OVL)/__set_thread_area.s $(MUSL_DIR)/Makefile
	bash tools/build_musl.sh
$(MUSL_CRT): $(MUSL_LIB) ;
musl: $(MUSL_LIB)

# NET-B: build the lwIP 2.2.1 core (third-party, -w) into build/lwip/liblwip.a
# from the pinned submodule + the PRADYOS port headers (third_party/lwip-port/).
# Archive only — linked into the kernel once the netif/net layer lands (NET-B 2+).
LWIP_DIR  := third_party/lwip
LWIP_PORT := third_party/lwip-port
LWIP_LIB  := build/lwip/liblwip.a
$(LWIP_LIB): tools/build_lwip.sh $(LWIP_PORT)/lwipopts.h $(LWIP_PORT)/arch/cc.h $(LWIP_PORT)/arch/sys_arch.h $(LWIP_DIR)/src/core/init.c
	bash tools/build_lwip.sh
lwip: $(LWIP_LIB)

# Build the NEXUS kernel: 64-bit entry stub (NASM) + C main, linked flat at
# 0x10000 and objcopied to a raw binary the bootloader loads verbatim.
kernel: $(KERNEL_BIN)

# DDR-822: the user-source prerequisites are a WILDCARD, not a hand-written list.
#
# They used to be enumerated as $(USER_*_SRC) variables. That list named 17 of
# the 31 files in user/, so editing any of the other 14 — including every crypto
# probe (sha256test, hkdftest, lockboxtest, sigpipetest, x25519test) — did NOT
# rebuild the kernel image, and the next gate run silently tested the PREVIOUS
# binary. That is the DDR-791 stale-artefact trap built into the build system,
# and it cost a full misdiagnosis of DDR-820 before it was found.
#
# A wildcard cannot go stale when a probe is added. Nothing here should ever be
# a list a human has to remember to extend.
# DDR-825: the probe ELFs link kernel/crypto sources too, and the Makefile
# itself decides what gets linked. Neither was a prerequisite, so editing a
# crypto source — or the recipe that links it — produced "Nothing to be done"
# and the next gate silently tested the PREVIOUS binary. That is DDR-822 exactly,
# one directory over: the wildcard fixed user/ and stopped there.
#
# Caught when adding fe25519.c: `make image` reported success, build/fe25519_user.o
# did not exist, and the .elf on disk was the pre-change one.
USER_ALL_SRCS := $(wildcard user/*.c) $(wildcard user/*.h) \
                 $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.h) \
                 Makefile

# DDR-833: kernel HEADERS are prerequisites too. Without this, editing a header
# (e.g. kernel/aether/aether.h) leaves `make image` reporting "Nothing to be done"
# and every gate then tests the PREVIOUS binary. That is DDR-822/825 a third time:
# the wildcard fixed user/, then kernel/crypto/, and stopped there both times.
KERNEL_HS := $(wildcard kernel/*.h) $(wildcard kernel/*/*.h) $(wildcard kernel/*/*/*.h)
# DDR-835: kernel SOURCES as a wildcard too. KERNEL_CS is a hand-maintained
# list, so a NEW .c file compiles (its rule is explicit) but never triggers a
# rebuild — `make image` says "Nothing to be done" and every gate tests the
# previous binary. That is DDR-822/825/833 a fourth time, and the fourth list
# to be fixed one entry at a time. A wildcard cannot go stale.
KERNEL_ALL_CS := $(wildcard kernel/*.c) $(wildcard kernel/*/*.c) $(wildcard kernel/*/*/*.c)

$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_ALL_CS) $(KERNEL_HS) $(KERNEL_LD) $(USER_ALL_SRCS) $(USER_C_LD) $(MUSL_LIB) $(MUSL_CRT) $(LWIP_LIB) third_party/lwip-port/lwip_port.c $(USER_LD)
	@mkdir -p build
	# Phase 5a: build the freestanding ring-3 programs and link each as its own
	# static ELF at 0x8000000000 (W^X: one R+X segment). user_image.asm then
	# incbin's both ELFs, so this MUST precede that assembly step.
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_SRC) -o build/hello.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ELF) build/hello.o
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_WX_SRC) -o build/wxviol.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_WX_ELF) build/wxviol.o
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_SYS_SRC) -o build/systest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SYS_ELF) build/systest.o
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_EXEC_SRC) -o build/exectest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_EXEC_ELF) build/exectest.o
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_TLS_SRC) -o build/tlstest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_TLS_ELF) build/tlstest.o
	$(NASM) $(NASM_WERROR) -f elf64 $(USER_FPU_SRC) -o build/fputest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_FPU_ELF) build/fputest.o
	# PROC-D step 3: the first C program — compiled against the musl subset and
	# linked with crt1.o + libc.a using the 3-segment W^X C linker script.
	@bash tools/build/gen_nsi_header.sh   # DDR-869: NSI header, generated not copied
	$(CC) $(USER_C_CFLAGS) -c $(USER_CMUSL_SRC) -o build/cmusl.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/cmusl.o $(MUSL_LIB) -o $(USER_CMUSL_ELF)
	# 5d: pradyos-init (PID 1), same musl-C link recipe as cmusl.
	$(CC) $(USER_C_CFLAGS) -c $(USER_INIT_SRC) -o build/init.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/init.o $(MUSL_LIB) -o $(USER_INIT_ELF)
	# 5e: PRISM shell, same musl-C link recipe; embedded + loaded from SFS like init.
	$(CC) $(USER_C_CFLAGS) -c $(USER_PRISM_SRC) -o build/prism.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/prism.o $(MUSL_LIB) -o $(USER_PRISM_ELF)
	# L6: AETHER daemon + agent template, same musl-C link recipe; embedded below
	# and loaded by the kernel (daemon from SFS like PRISM, agent via elf_load).
	$(CC) $(USER_C_CFLAGS) -c $(USER_AETHERD_SRC) -o build/aether_daemon.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/aether_daemon.o $(MUSL_LIB) -o $(USER_AETHERD_ELF)
	$(CC) $(USER_C_CFLAGS) $(USER_AGENT_DEFS) -c $(USER_AGENT_SRC) -o build/agent_base.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/agent_base.o $(MUSL_LIB) -o $(USER_AGENT_ELF)
	$(CC) $(USER_C_CFLAGS) -c $(USER_INPUT_SRC) -o build/inputtest.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/inputtest.o $(MUSL_LIB) -o $(USER_INPUT_ELF)
	$(CC) $(USER_C_CFLAGS) -c $(USER_COMP_SRC) -o build/compositor.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/compositor.o $(MUSL_LIB) -o $(USER_COMP_ELF)
	$(CC) $(USER_C_CFLAGS) -c $(USER_SURF_SRC) -o build/surfacetest.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/surfacetest.o $(MUSL_LIB) -o $(USER_SURF_ELF)
	# DDR-729/730: freestanding (no musl) so each stays a few KiB inside the kernel
	# image budget — links against user.ld's single R+X segment (no writable globals).
	$(CC) $(USER_C_CFLAGS) -c $(USER_SURFDESTROY_SRC) -o build/surfdestroytest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SURFDESTROY_ELF) build/surfdestroytest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_AGENTMETRICS_SRC) -o build/agentmetricstest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_AGENTMETRICS_ELF) build/agentmetricstest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_CAPNET_SRC) -o build/capnettest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_CAPNET_ELF) build/capnettest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_ROOTMNT_SRC) -o build/rootmounttest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ROOTMNT_ELF) build/rootmounttest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_FSRM_SRC) -o build/fsrmtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_FSRM_ELF) build/fsrmtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_FAT32MC_SRC) -o build/fat32mctest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_FAT32MC_ELF) build/fat32mctest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_NETHAMMER_SRC) -o build/nethammer.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_NETHAMMER_ELF) build/nethammer.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_MODKEYS_SRC) -o build/modkeystest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_MODKEYS_ELF) build/modkeystest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_STACKD_SRC) -o build/stackdemand.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/stackdemand.o $(MUSL_LIB) -o $(USER_STACKD_ELF)
	$(CC) $(USER_C_CFLAGS) -c $(USER_FTRUNC_SRC) -o build/ftrunctest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_FTRUNC_ELF) build/ftrunctest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_RENAME_SRC) -o build/renametest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_RENAME_ELF) build/renametest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_BENCH_SRC) -o build/benchtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_BENCH_ELF) build/benchtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SYSINFO_SRC) -o build/sysinfotest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SYSINFO_ELF) build/sysinfotest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_TIME_SRC) -o build/timetest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_TIME_ELF) build/timetest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_DMESG_SRC) -o build/dmesgtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_DMESG_ELF) build/dmesgtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_KILL_SRC) -o build/killtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_KILL_ELF) build/killtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SETNAME_SRC) -o build/setnametest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SETNAME_ELF) build/setnametest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_FUZZ_SRC) -o build/syscallfuzz.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_FUZZ_ELF) build/syscallfuzz.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_EGAUD_SRC) -o build/egressaudittest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_EGAUD_ELF) build/egressaudittest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SOVEG_SRC) -o build/sovegresstest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SOVEG_ELF) build/sovegresstest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_PRIVNET_SRC) -o build/privacynettest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_PRIVNET_ELF) build/privacynettest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SIGPIPE_SRC) -o build/sigpipetest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SIGPIPE_ELF) build/sigpipetest.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/sha256.c -o build/sha256_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_SHA256_SRC) -o build/sha256test.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SHA256_ELF) build/sha256test.o build/sha256_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_LOCKBOX_SRC) -o build/lockboxtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_LOCKBOX_ELF) build/lockboxtest.o build/sha256_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/hkdf.c -o build/hkdf_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_HKDF_SRC) -o build/hkdftest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_HKDF_ELF) build/hkdftest.o build/hkdf_user.o build/sha256_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/fe25519.c -o build/fe25519_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/x25519.c -o build/x25519_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_X25519_SRC) -o build/x25519test.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_X25519_ELF) build/x25519test.o build/x25519_user.o build/fe25519_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/sha512.c -o build/sha512_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_SHA512_SRC) -o build/sha512test.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SHA512_ELF) build/sha512test.o build/sha512_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/aead.c -o build/aead_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_AEAD_SRC) -o build/aeadtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_AEAD_ELF) build/aeadtest.o build/aead_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/ed25519.c -o build/ed25519_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_ED25519_SRC) -o build/ed25519test.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ED25519_ELF) build/ed25519test.o build/ed25519_user.o build/fe25519_user.o build/sha512_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c kernel/crypto/acc.c -o build/acc_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_ACC_SRC) -o build/acctest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ACC_ELF) build/acctest.o build/acc_user.o build/x25519_user.o build/fe25519_user.o build/hkdf_user.o build/sha256_user.o build/aead_user.o build/ed25519_user.o build/sha512_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -Ikernel/aether -c kernel/aether/ags.c -o build/ags_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -Ikernel/aether -c $(USER_AGS_SRC) -o build/agstest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_AGS_ELF) build/agstest.o build/ags_user.o build/ed25519_user.o build/fe25519_user.o build/sha256_user.o build/sha512_user.o
	$(CC) $(USER_C_CFLAGS) -Ikernel/crypto -c $(USER_ACCROT_SRC) -o build/accrottest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ACCROT_ELF) build/accrottest.o build/x25519_user.o build/fe25519_user.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_VAULT_SRC) -o build/vaulttest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_VAULT_ELF) build/vaulttest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_AMEM_SRC) -o build/agentmemtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_AMEM_ELF) build/agentmemtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_CKPT_SRC) -o build/ckpttest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_CKPT_ELF) build/ckpttest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SDEP_SRC) -o build/spawndepthtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SDEP_ELF) build/spawndepthtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_DAG_SRC) -o build/actiondagtest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_DAG_ELF) build/actiondagtest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_CRW_SRC) -o build/coderewritetest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_CRW_ELF) build/coderewritetest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_ACH_SRC) -o build/auditchaintest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_ACH_ELF) build/auditchaintest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_INV_SRC) -o build/invarianttest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_INV_ELF) build/invarianttest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_RTCMONO_SRC) -o build/rtcmonotest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_RTCMONO_ELF) build/rtcmonotest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_METRIC_SRC) -o build/metrictest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_METRIC_ELF) build/metrictest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_SFSROOT_SRC) -o build/sfsroottest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_SFSROOT_ELF) build/sfsroottest.o
	$(CC) $(USER_C_CFLAGS) -c $(USER_BIGWRITE_SRC) -o build/bigwritetest.o
	$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_BIGWRITE_ELF) build/bigwritetest.o
	@for e in $(USER_ELF) $(USER_WX_ELF) $(USER_SYS_ELF) $(USER_EXEC_ELF) $(USER_TLS_ELF) $(USER_FPU_ELF) $(USER_CMUSL_ELF) $(USER_INIT_ELF) $(USER_PRISM_ELF) $(USER_AETHERD_ELF) $(USER_AGENT_ELF) $(USER_INPUT_ELF) $(USER_COMP_ELF) $(USER_SURF_ELF) $(USER_SURFDESTROY_ELF) $(USER_AGENTMETRICS_ELF) $(USER_CAPNET_ELF) $(USER_ROOTMNT_ELF) $(USER_FSRM_ELF) $(USER_FAT32MC_ELF) $(USER_NETHAMMER_ELF) $(USER_MODKEYS_ELF) $(USER_FTRUNC_ELF) $(USER_RENAME_ELF) $(USER_STACKD_ELF) $(USER_BENCH_ELF) $(USER_SYSINFO_ELF) $(USER_TIME_ELF) $(USER_DMESG_ELF) $(USER_KILL_ELF) $(USER_SETNAME_ELF) $(USER_FUZZ_ELF) $(USER_SFSROOT_ELF) $(USER_BIGWRITE_ELF) $(USER_METRIC_ELF) $(USER_RTCMONO_ELF) $(USER_SOVEG_ELF) $(USER_EGAUD_ELF) $(USER_PRIVNET_ELF) $(USER_SIGPIPE_ELF) $(USER_SHA256_ELF) $(USER_LOCKBOX_ELF) $(USER_HKDF_ELF) $(USER_X25519_ELF) $(USER_SHA512_ELF) $(USER_AEAD_ELF) $(USER_ED25519_ELF) $(USER_ACC_ELF); do test "$$(wc -c < $$e)" -le 262144 || { echo "$$e exceeds 256 KiB (EXEC_MAX user-ELF budget)"; exit 1; }; done
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/user_image.asm    -o build/user_image.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/boot.asm          -o build/boot.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/cpu.asm           -o build/cpu.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/isr.asm           -o build/isr.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/context.asm       -o build/context.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/fast_memcpy.asm   -o build/fast_memcpy.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/ipc_copy.asm      -o build/ipc_copy.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/syscall_entry.asm -o build/syscall_entry.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/usermode.asm      -o build/usermode.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/ap_boot.asm       -o build/ap_boot.o
	$(CC) $(KCFLAGS) -c kernel/main.c          -o build/main.o
	$(CC) $(KCFLAGS) -c kernel/console.c       -o build/console.o
	$(CC) $(KCFLAGS) -c kernel/idt.c           -o build/idt.o
	$(CC) $(KCFLAGS) -c kernel/irq.c           -o build/irq.o
	$(CC) $(KCFLAGS) -c kernel/mm/pmm.c        -o build/pmm.o
	$(CC) $(KCFLAGS) -c kernel/mm/kheap.c      -o build/kheap.o
	$(CC) $(KCFLAGS) -c kernel/mm/vmm.c        -o build/vmm.o
	$(CC) $(KCFLAGS) -c kernel/mm/vmm_cow.c    -o build/vmm_cow.o
	$(CC) $(KCFLAGS) -c kernel/mm/uaccess.c    -o build/uaccess.o
	$(CC) $(KCFLAGS) -c kernel/cap.c           -o build/cap.o
	$(CC) $(KCFLAGS) -c kernel/proc/sched.c    -o build/sched.o
	$(CC) $(KCFLAGS) -c kernel/proc/tss.c      -o build/tss.o
	$(CC) $(KCFLAGS) -c kernel/proc/fd.c       -o build/fd.o
	$(CC) $(KCFLAGS) -c kernel/proc/pipe.c     -o build/pipe.o
	$(CC) $(KCFLAGS) -c kernel/proc/epoll.c    -o build/epoll.o
	$(CC) $(KCFLAGS) -c kernel/proc/signal.c   -o build/signal.o
	$(CC) $(KCFLAGS) -c kernel/ipc/ipc.c       -o build/ipc.o
	$(CC) $(KCFLAGS) -c kernel/ipc/bcast.c     -o build/bcast.o
	$(CC) $(KCFLAGS) -c kernel/syscall/syscall.c -o build/syscall.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_io.c  -o build/sys_io.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_file.c -o build/sys_file.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_proc.c -o build/sys_proc.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_mmap.c -o build/sys_mmap.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_exec.c -o build/sys_exec.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_fork.c -o build/sys_fork.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_wait.c -o build/sys_wait.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_io_uring.c -o build/sys_io_uring.o
	$(CC) $(KCFLAGS) -c kernel/acpi/acpi.c       -o build/acpi.o
	$(CC) $(KCFLAGS) -c kernel/apic/lapic.c      -o build/lapic.o
	$(CC) $(KCFLAGS) -c kernel/apic/ioapic.c     -o build/ioapic.o
	$(CC) $(KCFLAGS) -c kernel/drivers/ahci/ahci.c -o build/ahci.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/e1000e.c -o build/e1000e.o
	$(CC) $(KCFLAGS) -c kernel/mm/numa.c -o build/numa.o
	$(CC) $(KCFLAGS) -c kernel/fs/pdrive/pdrive.c -o build/pdrive.o
	$(CC) $(KCFLAGS) -c kernel/arch/x86_64/pstate.c -o build/pstate.o
	$(CC) $(KCFLAGS) -c kernel/apic/smp.c        -o build/smp.o
	$(CC) $(KCFLAGS) -c kernel/apic/percpu.c     -o build/percpu.o
	$(CC) $(KCFLAGS) -c kernel/drivers/pcie/pcie.c           -o build/pcie.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_ring.c  -o build/virtio_ring.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio.c       -o build/virtio.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_pci.c   -o build/virtio_pci.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/blk.c             -o build/blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/virtio_blk.c      -o build/virtio_blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/ramdisk.c         -o build/ramdisk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/virtio_net.c     -o build/virtio_net.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/netbuf.c         -o build/netbuf.o
	$(CC) $(KCFLAGS) -c kernel/drivers/gpu/virtio_gpu.c     -o build/virtio_gpu.o
	$(CC) $(KCFLAGS) -c kernel/drivers/nvme/nvme.c          -o build/nvme.o
	$(CC) $(KCFLAGS) -c kernel/drivers/input/ps2kbd.c       -o build/ps2kbd.o
	$(CC) $(KCFLAGS) -c kernel/drivers/input/virtio_input.c -o build/virtio_input.o
	$(CC) $(KCFLAGS) -c kernel/drivers/rtc/rtc.c            -o build/rtc.o
	$(CC) $(KCFLAGS) -c kernel/drivers/fwcfg/fwcfg.c        -o build/fwcfg.o
	$(CC) $(KCFLAGS) -c kernel/crypto/sha256.c              -o build/sha256.o
	$(CC) $(KCFLAGS) -c kernel/crypto/sha512.c              -o build/sha512.o
	$(CC) $(KCFLAGS) -c kernel/crypto/fe25519.c             -o build/fe25519.o
	$(CC) $(KCFLAGS) -c kernel/crypto/x25519.c              -o build/x25519.o
	$(CC) $(KCFLAGS) -c kernel/crypto/hkdf.c                -o build/hkdf.o
	$(CC) $(KCFLAGS) -c kernel/crypto/aead.c                -o build/aead.o
	$(CC) $(KCFLAGS) -c kernel/crypto/ed25519.c             -o build/ed25519.o
	$(CC) $(KCFLAGS) -c kernel/crypto/acc.c                 -o build/acc.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_acc.c            -o build/sys_acc.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/aether/ags.c -o build/ags.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/syscall/sys_ags.c -o build/sys_ags.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/aether/vault.c -o build/vault.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/syscall/sys_vault.c -o build/sys_vault.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/aether/agentmem.c -o build/agentmem.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/syscall/sys_agentmem.c -o build/sys_agentmem.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/syscall/sys_checkpoint.c -o build/sys_checkpoint.o
	$(CC) $(KCFLAGS) -Ikernel/aether -c kernel/syscall/sys_rewrite.c -o build/sys_rewrite.o
	$(CC) $(KCFLAGS) -Ikernel/aether -Ikernel/crypto -c kernel/syscall/sys_audit.c -o build/sys_audit.o
	$(CC) $(KCFLAGS) -c kernel/drivers/rng/virtio_rng.c     -o build/virtio_rng.o
	$(CC) $(KCFLAGS) -c kernel/fs/vfs/vfs.c                  -o build/vfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/fat32/fat32.c             -o build/fat32.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/sfs.c                 -o build/sfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/lz4.c                 -o build/lz4.o
	$(CC) $(KCFLAGS) -c kernel/fs/ext4/ext4.c              -o build/ext4.o
	$(CC) $(KCFLAGS) -c kernel/exec/elf.c                  -o build/elf.o
	$(CC) $(KCFLAGS) -c kernel/string.c        -o build/string.o
	$(CC) $(KCFLAGS) -c kernel/arch/x86_64/cpu_mitigations.c -o build/cpu_mitigations.o
	$(CC) $(KCFLAGS) -c kernel/vdso/vdso_page.c -o build/vdso_page.o
	$(CC) $(KCFLAGS) -c kernel/aether/metric_page.c -o build/metric_page.o
	$(CC) $(KCFLAGS) -c kernel/aether/aether.c            -o build/aether.o
	$(CC) $(KCFLAGS) -c kernel/aether/aether_queue.c      -o build/aether_queue.o
	$(CC) $(KCFLAGS) -c kernel/aether/aether_audit.c      -o build/aether_audit.o
	$(CC) $(KCFLAGS) -c kernel/aether/aether_mem.c        -o build/aether_mem.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_aether.c       -o build/sys_aether.o
	$(CC) $(KCFLAGS) -I$(LWIP_PORT) -c kernel/syscall/sys_socket.c -o build/sys_socket.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_fb.c            -o build/sys_fb.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_input.c        -o build/sys_input.o
	$(CC) $(KCFLAGS) -c kernel/syscall/sys_surface.c      -o build/sys_surface.o
	# NET-B: the lwIP-port glue (first-party, -Werror) — lwIP headers via -isystem
	# (no warnings from them), our shims via -I, -nostdlibinc drops host glibc.
	$(CC) $(KCFLAGS) -nostdlibinc -I$(LWIP_PORT) -isystem $(LWIP_DIR)/src/include -c third_party/lwip-port/lwip_port.c -o build/lwip_port.o
	$(LD) -nostdlib -T $(KERNEL_LD) -o $(KERNEL_ELF) $(KERNEL_OBJS) $(LWIP_LIB)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_BIN)
	@test "$$(wc -c < $(KERNEL_BIN))" -le 1572864 || { echo "kernel.bin exceeds 1.5 MiB — the stage-2 read window (48x64 sectors from LBA 17; DDR-733, raised by DDR-827 then DDR-960). Raise the chunk count in boot/stage2/stage2.asm. The 2 MiB disk image does NOT need to grow until 63 chunks, but the 2 MiB PT_HI runtime ceiling checked next DOES bound this: at 48 chunks a full window plus BSS still fits with 372,864 B to spare, and past ~56 it does not. Beyond that, extend PT_HI in BOTH boot/stage2/stage2.asm and boot/uefi/loader.c."; exit 1; }
	@end=$$($(NM) $(KERNEL_ELF) | awk '$$3 == "__bss_end" { print $$1 }'); \
	 phys=$$(( 0x$$end - 0xFFFFFFFF80000000 + 0x400000 )); \
	 test $$phys -le $$(( 0x600000 )) || { echo "kernel image+BSS ends at phys $$phys, past 0x600000 — the 2 MiB PT_HI higher-half span (DDR-733). Extend PT_HI in boot/stage2/stage2.asm (second PT) before growing further."; exit 1; }
	@echo "kernel: $(KERNEL_BIN) ($$(wc -c < $(KERNEL_BIN)) bytes)"

# Lay the three artifacts onto a 1 MiB raw disk at fixed LBAs:
#   LBA 0  stage1 (512 B MBR)   LBA 1  stage2 (<= 16 sectors)   LBA 17  kernel
# Stage 1 loads 16 sectors of Stage 2; Stage 2 bounce-loads 48x64 sectors of the
# kernel from LBA 17 to 4 MiB (DDR-733, window raised by DDR-960) — LBAs
# hard-coded in the asm, match here.
image: $(IMG)

$(IMG): $(STAGE1_SRC) $(STAGE2_SRC) $(KERNEL_BIN)
	@mkdir -p build
	$(NASM) $(NASM_WERROR) -f bin $(STAGE1_SRC) -o $(STAGE1_BIN)
	@test "$$(wc -c < $(STAGE1_BIN))" -eq 512 || { echo "stage1.bin is not 512 bytes (got $$(wc -c < $(STAGE1_BIN)))"; exit 1; }
	$(NASM) $(NASM_WERROR) -f bin $(STAGE2_SRC) -o $(STAGE2_BIN)
	@test "$$(wc -c < $(STAGE2_BIN))" -le 8192 || { echo "stage2.bin exceeds 8 KiB; Stage 1 only loads 16 sectors"; exit 1; }
	truncate -s 2M $(IMG)          # DDR-827: 1M->2M. DDR-960: this does NOT have to
	                               # move with every chunk-count raise — from LBA 17
	                               # a 2 MiB image holds 2,088,448 B, so it covers up
	                               # to 63 chunks. The 48-chunk window reads to LBA
	                               # 3089, clear of DDR-831's scratch sector 4095.
	dd if=$(STAGE1_BIN) of=$(IMG) bs=512 seek=0  conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$(IMG) bs=512 seek=1  conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$(IMG) bs=512 seek=17 conv=notrunc status=none
	@echo "image: $(IMG) (stage1 $$(wc -c < $(STAGE1_BIN))B, stage2 $$(wc -c < $(STAGE2_BIN))B, kernel $$(wc -c < $(KERNEL_BIN))B)"
# DDR-831: blk_selftest writes a scratch sector at BLK_SCRATCH_LBA (4095, the last
# sector of the 2 MiB image) and QEMU PERSISTS that write into the image file. If
# the kernel's on-disk extent ever reaches that sector, the self-test corrupts the
# kernel — including the probe ELFs incbin'd into .rodata — for every later boot.
# That was OPEN-11: the old literal LBA 1500 was chosen when the kernel was capped
# at 512 sectors, and DDR-827 grew the kernel to ~1666 sectors without anything
# failing. A comment cannot fail a build; this check can.
	@kend=$$(( 17 + ( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 ) )); \
	 test "$$kend" -lt 4095 || { \
	   echo "kernel reaches LBA $$kend, at or past the blk_selftest scratch sector 4095 (DDR-831)."; \
	   echo "The self-test would write into the kernel image and QEMU would persist it."; \
	   echo "Grow the image and move BLK_SCRATCH_LBA in kernel/main.c together."; exit 1; }
	@echo "image: kernel ends at LBA $$(( 17 + ( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 ) )), scratch sector 4095 — clear (DDR-831)"

# A FAT32 data disk (with known files) for the VFS/FAT32 self-test. 64 MiB is
# above the FAT32 minimum. Uses dosfstools (mkfs.fat) + mtools (mcopy/mmd).
# `fat-image` is phony so every FS gate starts from a FRESH volume — the kernel
# write test creates /KOUT.TXT, which must not already exist on a second run.
FAT_IMG := build/fat.img

# DDR-973: depends on the kernel build because that is what produces
# $(USER_CMUSL_ELF), which the recipe now mcopy's onto the volume. Without
# this a parallel `make -j smoke-...` could run the recipe before the ELF exists.
fat-image: $(KERNEL_BIN)
	@mkdir -p build
	dd if=/dev/zero of=$(FAT_IMG) bs=1M count=64 status=none
	mkfs.fat -F 32 -n PRADYOS $(FAT_IMG) >/dev/null
	printf 'PRADYOS filesystem works!' > build/hello.txt
	mcopy -i $(FAT_IMG) build/hello.txt ::/HELLO.TXT
	mmd   -i $(FAT_IMG) ::/DOCS
	printf 'nested file ok' > build/note.txt
	mcopy -i $(FAT_IMG) build/note.txt ::/DOCS/NOTE.TXT
	printf 'long name read works' > build/longname.txt
	mcopy -i $(FAT_IMG) build/longname.txt ::/LongFileName.txt
	# DDR-787: a file LARGER than PIPE_SIZE (4096), with distinct first/last
	# markers, so `cat /BIG8K.TXT | cat` can prove byte-exact pipe delivery. Before
	# blocking pipe writes this truncated at 4096 and BIGTAIL-e5v never arrived.
	# 200 lines ~= 7.8 KiB: comfortably past PIPE_SIZE while staying quick enough
	# that smoke-shell keeps a wide margin inside its (unraised) 60 s window.
	printf 'BIGHEAD-e5v\n' > build/big8k.txt
	for i in $$(seq 1 200); do printf 'pipe payload line %03d 0123456789abcdef\n' $$i >> build/big8k.txt; done
	printf 'BIGTAIL-e5v\n' >> build/big8k.txt
	mcopy -i $(FAT_IMG) build/big8k.txt ::/BIG8K.TXT
	# DDR-973: the FAT32 multi-cluster read fixtures. mkfs.fat -F32 over 64 MiB
	# lands on 1 sector per cluster, so 64 KiB is 128 clusters and the 30,488-byte
	# cmusl image is 60 -- both well past the 16-cluster reach of /BIG8K.TXT.
	# byte n = (7n + 3 + 31*(n >> 8)) & 0xFF. The 31*(n>>8) term is load-bearing:
	# plain (7n+3)&0xFF has period 256, so with 512-byte clusters every cluster
	# would hold IDENTICAL bytes and a chain repeat would read back perfectly --
	# a mutant proving exactly that passed the first cut of this gate (DDR-973 sec.6).
	# 31 is invertible mod 256, so all 256 blocks of the 64 KiB file are distinct.
	# python3 (not printf) because the data contains NULs, which command
	# substitution would eat; mkfs.fat + mtools are already hard deps of this recipe.
	python3 -c "import sys; sys.stdout.buffer.write(bytes(((7*n+3+31*(n>>8))&0xFF) for n in range(65536)))" > build/bigpat.bin
	mcopy -i $(FAT_IMG) build/bigpat.bin ::/BIGPAT.BIN
	# Arm C's target: the LARGE musl-C ELF ADR-024 sec.D5 named. Copied, not rebuilt,
	# so the gate execve's the same image the kernel already loads from SFS at boot.
	mcopy -i $(FAT_IMG) $(USER_CMUSL_ELF) ::/CMUSL.ELF
	# DDR-761: the AETHER boot policy moved OFF the FAT boot volume — the daemon now
	# reads /etc/aether/config on the SFS root (kernel-provisioned; DDR-760). The old
	# FAT /AETHER.CFG (DDR-732/734) is retired here.
	@echo "fat: $(FAT_IMG) (FAT32, /HELLO.TXT, /DOCS/NOTE.TXT, /LongFileName.txt, /BIG8K.TXT, /BIGPAT.BIN, /CMUSL.ELF)"

# A blank 16 MiB disk for the SFS self-test; the kernel formats it as SFS in
# place (in-kernel mkfs) then mounts it. Phony so each gate starts blank.
SFS_IMG := build/sfs.img

sfs-image:
	@mkdir -p build
	dd if=/dev/zero of=$(SFS_IMG) bs=1M count=16 status=none
	@echo "sfs: $(SFS_IMG) (16 MiB blank — kernel formats in place)"

# Host SFS image writer + read-back verifier (DDR-767). Both #include the kernel's
# sfs.h so their on-disk structs + FNV-1a name hash are byte-identical to the
# kernel reader — no format drift. Built with the host cc (like mkfs.fat/mtools).
MKFS_SFS      := build/mkfs.sfs
SFS_READBACK  := build/sfs_readback
MKFS_SFS_IMG  := build/mkfs_sfs.img
SFS_PERSIST_MARK := PRADYOS SFS persistence marker: DDR-767 OK

$(MKFS_SFS): tools/mkfs_sfs/mkfs_sfs.c kernel/fs/sfs/sfs.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -Ikernel/fs/sfs -Ikernel/fs/pdrive -o $@ $<

$(SFS_READBACK): tools/mkfs_sfs/sfs_readback.c kernel/fs/sfs/sfs.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -Ikernel/fs/sfs -Ikernel/fs/pdrive -o $@ $<

# Host round-trip gate: mkfs.sfs writes an image provisioning /PERSIST.TXT, and
# sfs_readback (kernel sfs.h structs + the kernel's leaf-scan/inode/extent read
# path) recovers it byte-for-byte — proving mkfs output is kernel-decodable. The
# kernel-boots-and-reads end-to-end proof is DDR-768.
smoke-mkfs-sfs: $(MKFS_SFS) $(SFS_READBACK)
	@mkdir -p build
	printf '$(SFS_PERSIST_MARK)' > build/persist.txt
	$(MKFS_SFS) $(MKFS_SFS_IMG) --blocks 4096 --file PERSIST.TXT=build/persist.txt
	@out=$$($(SFS_READBACK) $(MKFS_SFS_IMG) PERSIST.TXT); \
	 echo "readback: $$out"; \
	 [ "$$out" = "$(SFS_PERSIST_MARK)" ] \
	   && echo "[smoke] PASS — mkfs.sfs host round-trip" \
	   || { echo "[smoke] FAIL — mkfs.sfs round-trip mismatch"; exit 1; }
# DDR-773: multi-leaf B+tree. 20 files = 41 slots > SFS_LEAF_MAX(14) -> 3 leaves
# under an internal root. Reading the FIRST, a MIDDLE and the LAST file proves
# the internal descend lands correctly at both edges of the separator range.
	@set -e; args=""; \
	 for i in $$(seq 0 19); do \
	   printf 'multileaf-content-%02d' $$i > build/ml$$i.txt; \
	   args="$$args --file F$$i.TXT=build/ml$$i.txt"; \
	 done; \
	 $(MKFS_SFS) build/mkfs_multi.img --blocks 4096 $$args; \
	 for i in 0 9 19; do \
	   got=$$($(SFS_READBACK) build/mkfs_multi.img F$$i.TXT); \
	   want=$$(printf 'multileaf-content-%02d' $$i); \
	   [ "$$got" = "$$want" ] || { echo "[smoke] FAIL — multi-leaf F$$i: '$$got' != '$$want'"; exit 1; }; \
	 done; \
	 echo "[smoke] PASS — mkfs.sfs multi-leaf B+tree (20 files, first/middle/last)"

# An ext4 disk populated at mkfs time (-d) with a known file, for the ext4
# read-only self-test. Forces 4 KiB blocks to match the kernel's page reads.
# Needs e2fsprogs (mkfs.ext4 with -d, 1.43+).
EXT4_IMG := build/ext4.img

ext4-image:
	@mkdir -p build/ext4root
	printf 'ext4 read works' > build/ext4root/EXT4.TXT
	rm -f $(EXT4_IMG)
	mkfs.ext4 -q -F -b 4096 -d build/ext4root $(EXT4_IMG) 16M
	@echo "ext4: $(EXT4_IMG) (16 MiB, ext4, /EXT4.TXT)"

# Kernel boot gate: proves the kernel boots and prints its sentinel. Deliberately
# does NOT depend on the FAT image — the kernel gate must not fail just because a
# host FS tool (mtools/dosfstools) is absent. boot_test.sh attaches build/fat.img
# only if it happens to exist; with no FS disk the self-test degrades cleanly to
# "no mountable filesystem" and the kernel sentinel still appears.
smoke: $(IMG)
	@# DDR-892 (item 27): S3 DISCOVERY and the P-state refusal. The raw
	@# occurrence counter is asserted alongside parsed= because it is what
	@# separates "the scanner is broken" from "the report ran too early" —
	@# the actual bug found here, where acpi_s3_available() was read before
	@# acpi_power_init() had scanned the DSDT.
	EXTRA_SENTINEL="$$(printf '[acpi] DSDT _S3_ occurrences=1 parsed=1\n[acpi] S3 available\n[acpi] S3 refused: no resume path\n[pstate] EIST unsupported')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-785: host-only self-test of the boot_test.sh harness itself (no QEMU, no
# kernel — a stub qemu on PATH replays scripted serial logs). Proves early exit
# happens when eligible AND that it cannot mask a late forbidden pattern, which
# is the one way the change could have silently weakened all 91 boot gates.
smoke-selftest:
	bash tools/qemu_runner/selftest.sh

# Filesystem gate: builds the FAT32 data disk and asserts BOTH the kernel
# sentinel AND the FAT32 read self-test line — real end-to-end FS coverage.
# Needs dosfstools (mkfs.fat) + mtools (mcopy); see setup_toolchain.sh.
# DDR-783: TIMEOUT_S=60, not the harness default 30. This gate asserts the LAST
# sentinel in the whole boot chain — measured at t=24.3s locally, i.e. only 19%
# margin under a 30s window, which flaked on a slower CI runner (run 30192189559).
# smoke-user already runs the same 'compress/readback/tag OK' assertion at 60s.
# This cannot mask a hang: boot_test.sh greps AFTER the window either way.
smoke-fs: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'msix vec=56\nPRADYOS filesystem works!\nnested file ok\nlong name read works\n[rtc] 20\nkernel wrote this\ncreated+deleted /TMP.TXT OK\ncreate/lookup OK\nbyte-exact OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK\n[sfs] freelist persist OK\n[pdrive] workspace OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-955: proves the bounded-wait path is present and that its deadline does
# NOT fire on healthy I/O. The FORBIDDEN sentinels are the discriminating half
# (R8): a spurious 500-tick expiry turns a passing boot into a failure here,
# which is the regression this change could plausibly introduce.
smoke-blk-timeout: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'msix vec=56\nPRADYOS filesystem works!\n[pdrive] workspace OK')" \
	    FORBIDDEN_SENTINEL="$$(printf '[vblk] compl wait timeout\n[vblk] slot wait timeout')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Read-write FS gate with ADVERSARIAL HOST-SIDE VALIDATION: boot the kernel (it
# creates+writes /KOUT.TXT and create+deletes /TMP.TXT on the FAT32 disk), then
# on the host run fsck.fat to prove the volume is still consistent and mdir/mtype
# to prove the kernel-written file persisted with the right contents. QEMU writes
# the modified disk image back to build/fat.img, so the host sees the kernel's
# changes. Needs dosfstools (fsck.fat) + mtools (mdir/mtype).
smoke-fs-rw: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'kernel wrote this\ncreated+deleted /TMP.TXT OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)
	@echo "[fs-rw] host fsck.fat (consistency after kernel writes):"
	fsck.fat -n -v $(FAT_IMG) | tail -n 20
	@echo "[fs-rw] host mdir (root listing — expect KOUT.TXT, no TMP.TXT):"
	mdir -i $(FAT_IMG) ::/
	@echo "[fs-rw] host mtype /KOUT.TXT (expect 'kernel wrote this'):"
	mtype -i $(FAT_IMG) ::/KOUT.TXT
	@echo "[fs-rw] PASS — kernel-written file persisted and volume is consistent."

# SFS read-write gate: the kernel formats a blank disk as SFS, builds a CoW
# B+tree (create/lookup), and does a 64 KiB extent write -> read-back -> grow.
# Asserts the SFS-specific self-test lines (create/lookup + byte-exact + grow).
smoke-fs-sfs-rw: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'create/lookup OK\nbyte-exact OK\nto 69632 OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK\n[sfs] freelist persist OK\n[pdrive] workspace OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SFS hierarchical-directory gate (DDR-738): the kernel builds /etc/aether/config
# (mkdir -p intermediates), reads it back from a fresh path walk, rejects opening
# a directory as a file and a missing intermediate, and enumerates each level.
smoke-sfs-dirs: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf '[sfs] hier dirs OK')" \
	FORBIDDEN_SENTINEL="hier dirs FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SFS unlink/rmdir gate (DDR-741): the kernel removes a file (tombstone),
# re-creates the freed name, refuses rmdir on a non-empty dir, removes it
# leaf-first, and confirms readdir no longer lists it.
smoke-sfs-unlink: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf '[sfs] unlink/rmdir OK')" \
	FORBIDDEN_SENTINEL="unlink/rmdir FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# ext4 read-only gate: a host-built ext4 disk with /EXT4.TXT; the kernel mounts
# it (4th disk) and reads the file back. Asserts the ext4 self-test line.
smoke-fs-ext4: $(IMG) fat-image sfs-image ext4-image
	TIMEOUT_S=120 EXTRA_SENTINEL='ext4 read works' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-process root-mount gate (DDR-739): with the ext4 disk attached, a ring-3
# probe is spawned with its root_mnt set to ext4 (not the FAT default). It opens
# /EXT4.TXT (ext4-only) AND fails /HELLO.TXT (FAT-only) -> proves SYS_OPEN
# resolves against the SELECTED root. Needs the ext4 disk (like smoke-fs-ext4).
smoke-rootmount: $(IMG) fat-image sfs-image ext4-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ROOTMOUNT_OK ext4')" \
	FORBIDDEN_SENTINEL="ROOTMOUNT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-744 ring-3 file-lifecycle gate: the SFS-rooted fsrmtest probe creates a
# file via SYS_OPEN(O_CREAT), reads it back, SYS_UNLINKs it, confirms it is gone,
# and re-unlinks (clean error). Proves O_CREAT + SYS_UNLINK across the syscall
# boundary against the writable SFS root. PRADYOS_FSRM_OK on all-pass.
# DDR-866 (Group 3 item 20) ftruncate gate. Opt-in via the DDR-804 fw_cfg probe
# selector: the probe CREATES A FILE ON THE SHARED SFS ROOT, so running it every
# boot would change what every other SFS gate observes.
#
# The FORBIDDEN sentinel gives the gate teeth. The probe prints a specific reason
# per failed case, so a shrink that silently zeroed the surviving bytes, or a
# grow that handed back stale block content, fails here rather than passing a
# length-only check.
# DDR-870 (Group 8 items 44/45): perf benchmark. Opt-in via DDR-804 — it issues
# thousands of syscalls and would perturb the timing-sensitive gates if it ran
# on every boot.
#
# The gate asserts the benchmark RAN and produced self-consistent numbers, not
# that any particular figure was hit. Under QEMU TCG the guest TSC counts
# emulated time, so a hard cycle threshold would be asserting a property the
# measurement cannot support — it would fail on a faster host and pass on a
# slower one, for reasons unrelated to the code.
smoke-bench: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=bench \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_BENCH_OK')" \
	FORBIDDEN_SENTINEL="BENCH FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-875 (Group 4 item 23): AHCI. Attaches an ich9-ahci controller with a real
# SATA disk and requires the driver to enumerate it, IDENTIFY it, and report a
# non-zero sector count. The FORBIDDEN sentinel catches the failure that would
# otherwise look like success: a controller that is found but whose port setup
# or IDENTIFY quietly fails still prints a 'controller ready' line.
AHCI_IMG := build/ahci.img
ahci-image:
	@mkdir -p build
	dd if=/dev/zero of=$(AHCI_IMG) bs=1M count=8 status=none
	@echo "ahci: $(AHCI_IMG) (8 MiB blank SATA disk)"

# DDR-876 (Group 4 item 25): Intel e1000e. Attaches a real e1000e NIC and
# requires the driver to map BAR0, read a MAC out of the Receive Address
# registers, and bring the ring up. The MAC assertion is what makes this more
# than a probe test: QEMU's default e1000e MAC begins 52:54:00, so a driver
# that found the device but read the wrong register would print zeros and fail.
#
# 'RX OK' is the assertion that matters most: it means a broadcast ARP went out
# and slirp's reply came back through the RX ring. That one round trip is the
# only thing that proves the descriptor rings, the DD bit and the tail pointer
# are right — all three can be wrong while the device still probes cleanly.
# DDR-882 (Group 3 item 17a): SRAT NUMA topology, against a QEMU machine that
# genuinely has two memory nodes.
#
# 'node1 mem=256MiB' is the assertion that carries the weight. A parser that
# finds SRAT but reads the base/length fields at the wrong offsets still
# reports a node COUNT, and only an exact byte total shows it walked the
# sub-tables correctly. node0 is 249MiB, not 250: its low range is split around
# the legacy hole, which is also why ranges=3 for two nodes. The 250M/262M split
# is deliberate — see boot_test.sh: a 256/256 split is max-order aligned and
# leaves item 17b straddle-splitting untested.
#
# NO FORBIDDEN SENTINEL, deliberately. The rejection check is folded into the
# required line as 'rejected=0', so the DDR-785 early exit stays eligible and
# the gate stops as soon as numa_init() has printed — before lapic_init()
# brings up the second vCPU. A FORBIDDEN pattern would disable early exit and
# expose this gate to the unresolved lost-thread defect (DDR-880).
# DDR-882 (Group 3 item 17b): node-aware ALLOCATION, not just topology.
#
# Parsing SRAT proves nothing about allocation — an allocator that ignores its
# node argument passes every assertion in smoke-numa. This asks node 1 for a
# frame and requires the address to land inside node 1's range.
#
# The rebucket line is asserted too, but only on what is STABLE. node0=59904 is
# structural — node 0 is 250 MiB and the managed range starts at 16 MiB, so it
# contributes 234 MiB = 59904 pages. The
# block count and node1's exact page count are NOT asserted: they shift with how
# much was allocated before the rebucket runs (194/65320 under the probe vs
# 198/65323 without it), so pinning them would be a gate that fails on unrelated
# allocation changes.
#
# 'node1=6' still has teeth: a rebucket that filed everything onto node 0 leaves
# node1=0, and one that mis-split straddling blocks moves node0 off 61440.
# ---- DDR-886 (Group 4 item 22): UEFI boot path ---------------------------
# clang emits PE32+ directly and lld-link is already installed, so no EDK2 or
# gnu-efi dependency is introduced (DDR-886 section 2). -Werror as everywhere.
UEFI_EFI := build/BOOTX64.EFI
ESP_IMG  := build/esp.img

$(UEFI_EFI): boot/uefi/loader.c boot/uefi/efi.h
	clang --target=x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
	      -Wall -Wextra -Werror -c boot/uefi/loader.c -o build/uefi_loader.obj
	lld-link-18 -subsystem:efi_application -entry:efi_main -nodefaultlib \
	      build/uefi_loader.obj -out:$(UEFI_EFI)

# A FAT ESP carrying the loader at the removable-media path plus the SAME
# kernel.bin the legacy path uses. Byte-identical on purpose: nothing in the
# kernel is conditional on which loader ran.
esp-image: $(UEFI_EFI) $(KERNEL_BIN)
	dd if=/dev/zero of=$(ESP_IMG) bs=1M count=48 status=none
	mkfs.fat -F 32 -n PRADYOSESP $(ESP_IMG) >/dev/null
	mmd -i $(ESP_IMG) ::/EFI ::/EFI/BOOT
	mcopy -i $(ESP_IMG) $(UEFI_EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(ESP_IMG) $(KERNEL_BIN) ::/KERNEL.BIN
	@echo "esp: $(ESP_IMG) (BOOTX64.EFI + KERNEL.BIN)"

# The sentinel is IDENTICAL to every other boot gate on purpose: it proves the
# same unmodified kernel reaches the same state through a completely different
# loader. A UEFI-specific sentinel would let the two paths drift while both
# looked green. '[uefi] handoff' is required too so a failure can be attributed
# to the loader rather than the kernel.
#
# The E820 entry COUNT is required too, and that is what proves the HANDOFF.
# Mutation testing found the gate blind without it: handing the kernel a
# deliberately wrong boot_info pointer still reached NEXUS KERNEL OK, because
# that sentinel prints before the memory map is used for anything. 16 is the
# merged count for this OVMF machine; garbage at a wrong pointer will not be 16.
# ---- DDR-896 (Group 9 item 48): hybrid BIOS + UEFI ISO -------------------
# Multiboot2 is SUPERSEDED (owner-approved, DDR-896). The ISO carries the two
# loaders this project already proved independently rather than adding a third
# handoff contract that would hand control in 32-bit protected mode.
#
# BIOS arm: floppy emulation at 2.88 MiB. El Torito then presents pradyos.img
# to the BIOS as drive 0 with 512-byte sectors, which is exactly what stage1
# already reads. No-emulation boot would hand it 2048-byte CD sectors and every
# LBA in stage1/stage2 would address the wrong place.
#
# UEFI arm: build/esp.img verbatim — the same FAT ESP smoke-uefi already boots.
ISO_IMG   := build/pradyos.iso
ISO_HD    := build/pradyos-hd.img

# El Torito HARD-DISK emulation: drive 0x80, 512-byte sectors, EDD supported.
# Floppy emulation was tried first and MEASURED to fail — emulated floppies do
# not implement INT 13h AH=42h, and stage1 issues exactly that, printing
# 'PRADYOS S1: DISK READ ERROR'. No-emulation is worse: 2048-byte CD sectors
# would put every LBA four times too deep. Hard-disk emulation needs an MBR
# partition table, which mk_hdimg.py writes into a COPY.
$(ISO_HD): $(IMG)
	python3 tools/build/mk_hdimg.py $(IMG) $(ISO_HD)

iso: $(ISO_HD) esp-image
	@rm -rf build/isoroot && mkdir -p build/isoroot/boot
	cp $(ISO_HD)     build/isoroot/boot/pradyos.img
	cp build/esp.img build/isoroot/boot/esp.img
	xorriso -as mkisofs \
	    -V PRADYOS \
	    -b boot/pradyos.img -hard-disk-boot \
	    -eltorito-alt-boot -e boot/esp.img -no-emul-boot \
	    -o $(ISO_IMG) build/isoroot
	@echo "iso: $(ISO_IMG) ($$(stat -c%s $(ISO_IMG)) bytes, BIOS+UEFI)"
smoke-iso-x86: iso
	@echo "[iso] BIOS arm..."
	@timeout 120 qemu-system-x86_64 -machine q35 -cdrom $(ISO_IMG) -boot d \
	    -no-reboot -display none -monitor none -serial file:build/iso_bios.log >/dev/null 2>&1 || true
	@grep -q 'NEXUS KERNEL OK' build/iso_bios.log || { echo "[iso] FAIL: BIOS arm did not reach the kernel"; tail -20 build/iso_bios.log; exit 1; }
	@echo "[iso] BIOS arm OK"
	@echo "[iso] UEFI arm..."
	@cp /usr/share/OVMF/OVMF_VARS_4M.fd build/iso_vars.fd
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,unit=1,file=build/iso_vars.fd \
	    -cdrom $(ISO_IMG) -boot d \
	    -no-reboot -display none -monitor none -serial file:build/iso_uefi.log >/dev/null 2>&1 || true
	@grep -q '\[uefi\] handoff' build/iso_uefi.log || { echo "[iso] FAIL: UEFI arm loader did not hand off"; tail -20 build/iso_uefi.log; exit 1; }
	@grep -q 'NEXUS KERNEL OK' build/iso_uefi.log || { echo "[iso] FAIL: UEFI arm did not reach the kernel"; tail -20 build/iso_uefi.log; exit 1; }
	@echo "[iso] UEFI arm OK — one ISO, both boot paths, same sentinel"

# DDR-972 (fixes DDR-971): the ISO must boot an OS, not just a kernel.
#
# smoke-iso-x86 asserts NEXUS KERNEL OK, which prints at line 30 of 145 — about
# 60 lines before userspace starts. DDR-971 measured an ISO that passed that
# gate and then idled forever at rqdepth=1 curpid=0 with no PRISM, no aetherd
# and no filesystem. This gate is what would have caught it, and what stops it
# reopening: it drives the SHELL over the ISO's serial console and asserts on
# what the commands actually DO.
#
# Fixtures differ from smoke-shell deliberately: the ISO root is a freshly
# formatted SFS ramdisk and starts EMPTY, so there is no /HELLO.TXT to list.
# The file this asserts on is one the gate creates itself, which is a stronger
# check anyway — it proves a write reached the root and was read back.
smoke-iso-userspace: iso
	@echo "[iso-user] booting the ISO and driving PRISM..."
	@SHIN=$$(mktemp -u /tmp/pradyos_iso.XXXXXX); rm -f build/iso_user.log; mkfifo "$$SHIN"; \
	( exec > "$$SHIN"; \
	  for i in $$(seq 1 600); do grep -q PRISM_READY build/iso_user.log 2>/dev/null && break; sleep 0.1; done; \
	  sleep 1; \
	  printf 'echo iso-live-marker-3f7\n'; sleep 0.6; \
	  printf 'uname\n'; sleep 0.6; \
	  printf 'free\n'; sleep 0.6; \
	  printf 'uptime\n'; sleep 0.6; \
	  printf 'ps\n'; sleep 0.6; \
	  printf 'echo iso-file-content-9k2 > /ISOTEST.TXT\n'; sleep 0.8; \
	  printf 'ls /\n'; sleep 0.8; \
	  printf 'cat /ISOTEST.TXT\n'; sleep 0.8; \
	  printf 'rm /ISOTEST.TXT\n'; sleep 0.8; \
	  printf 'exit\n'; sleep 0.5 ) & \
	timeout 180 qemu-system-x86_64 -machine q35 -cdrom $(ISO_IMG) -boot d \
	    -device virtio-gpu-pci \
	    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	    -no-reboot -display none -monitor none \
	    -serial stdio < "$$SHIN" > build/iso_user.log 2>/dev/null || true; \
	rm -f "$$SHIN"
	@# 1. the root actually mounted — the DDR-971 failure was its absence
	@grep -q 'ramdisk. formatted SFS' build/iso_user.log || { echo "[iso-user] FAIL: no ramdisk root"; tail -20 build/iso_user.log; exit 1; }
	@grep -qE '\[fs\] mounted' build/iso_user.log || { echo "[iso-user] FAIL: nothing mounted"; tail -20 build/iso_user.log; exit 1; }
	@# 2. userspace came up
	@grep -q 'PRISM_READY' build/iso_user.log || { echo "[iso-user] FAIL: PRISM never started"; tail -30 build/iso_user.log; exit 1; }
	@grep -q 'aetherd:' build/iso_user.log || { echo "[iso-user] FAIL: AETHER daemon never started"; tail -30 build/iso_user.log; exit 1; }
	@grep -q 'PRADYOS_AGENT_DONE' build/iso_user.log || { echo "[iso-user] FAIL: agent did not complete"; tail -30 build/iso_user.log; exit 1; }
	@# 3. the shell RESPONDS — echo proves the console read a command and ran it
	@grep -qF 'iso-live-marker-3f7' build/iso_user.log || { echo "[iso-user] FAIL: shell did not execute echo"; tail -30 build/iso_user.log; exit 1; }
	@# 4. introspection builtins return their documented SHAPES, not just exit 0
	@grep -qE 'uname: .*cpus='  build/iso_user.log || { echo "[iso-user] FAIL: uname"; exit 1; }
	@grep -qE 'mem: total=[0-9]+K free=[0-9]+K used=[0-9]+K' build/iso_user.log || { echo "[iso-user] FAIL: free"; exit 1; }
	@grep -qE 'uptime: [0-9]+s' build/iso_user.log || { echo "[iso-user] FAIL: uptime"; exit 1; }
	@grep -qE 'PID +PPID S U +CPUms +DISP NAME' build/iso_user.log || { echo "[iso-user] FAIL: ps header"; exit 1; }
	@# 5. a real write/read/delete round-trip on the ISO's own root. Three
	@#    assertions, none sufficient alone: the file must LIST (it was created),
	@#    its CONTENT must read back (the write reached the medium), and rm must
	@#    confirm (delete works). A root that mounted read-only would pass none.
	@grep -qE '(^|prism> )ISOTEST\.TXT$$' build/iso_user.log || { echo "[iso-user] FAIL: created file not listed"; tail -30 build/iso_user.log; exit 1; }
	@grep -qF 'iso-file-content-9k2' build/iso_user.log || { echo "[iso-user] FAIL: content did not read back"; tail -30 build/iso_user.log; exit 1; }
	@grep -qF 'rm: removed /ISOTEST.TXT' build/iso_user.log || { echo "[iso-user] FAIL: rm did not remove"; tail -30 build/iso_user.log; exit 1; }
	@# 6. the graphics stack initialises from the ISO when a GPU is present, and
	@#    a ring-3 client completes a surface lifecycle against it.
	@grep -q 'PRADYOS_GPU_FB_OK' build/iso_user.log || { echo "[iso-user] FAIL: no framebuffer from the ISO"; tail -30 build/iso_user.log; exit 1; }
	@grep -q 'PRADYOS_SURFACE_CLIENT_OK' build/iso_user.log || { echo "[iso-user] FAIL: surface lifecycle"; tail -30 build/iso_user.log; exit 1; }
	@# 7. lwIP carries real loopback traffic — not just "the NIC linked up".
	@grep -q 'PRADYOS_NET_LO_OK' build/iso_user.log || { echo "[iso-user] FAIL: no lwIP loopback traffic"; tail -30 build/iso_user.log; exit 1; }
	@if grep -qiE '\[panic\]|KERNEL PANIC' build/iso_user.log; then echo "[iso-user] FAIL: kernel panic"; tail -30 build/iso_user.log; exit 1; fi
	@echo "[iso-user] PASS — ISO boots a live OS: SFS root + PRISM + AETHER agent + write/read/delete round-trip"

# DDR-978: the old assertions were `[uefi] handoff` + the E820 line, and BOTH are
# true of a machine with ZERO PCI devices. That is exactly what this gate was
# passing on: under OVMF the kernel found no RSDP (firmware publishes it via the
# EFI Configuration Table, not the legacy 0xE0000 window), so no MCFG, so PCIe
# enumerated NOTHING -- no disk, no net, no GPU -- and no MADT, so no APs. The
# ISO "worked" only because DDR-972's ramdisk fallback fires on blk_count()==0,
# the very condition the defect creates. Fourth vacuous gate in this project
# (DDR-971, DDR-973 sec.6, DDR-880's harness-echo detector).
#
# The FORBIDDEN sentinels are the discriminating half: each is the literal line
# the broken path printed, so a regression fails here instead of passing.
smoke-uefi: esp-image
	TIMEOUT_S=90 QEMU_UEFI=1 \
	EXTRA_SENTINEL="$$(printf '[uefi] handoff\nNEXUS: E820 map, entries=0x0000000000000010\nACPI: RSDP from loader\nACPI: FADT ok\nPCIe: ECAM')" \
	FORBIDDEN_SENTINEL="$$(printf '[uefi] FATAL\nACPI: RSDP not found\nPCIe: no MCFG table\nACPI: loader RSDP rejected')" \
	    bash tools/qemu_runner/boot_test.sh $(ESP_IMG)

smoke-numa-alloc: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_NUMA=1 QEMU_PROBES=numaalloc \
	EXTRA_SENTINEL="$$(printf 'node0=59904 node1=6\n[numa] alloc node1 -> node1 OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-numa: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_NUMA=1 \
	EXTRA_SENTINEL="$$(printf '[numa] nodes=2 ranges=3 rejected=0\n[numa] node0 mem=249MiB\n[numa] node1 mem=262MiB')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-e1000e: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_E1000E=1 \
	EXTRA_SENTINEL="$$(printf '[e1000e] up, mac=52:54:00:\n[e1000e] TX OK\n[e1000e] RX OK n=')" \
	FORBIDDEN_SENTINEL="$$(printf '[e1000e] BAR0 unassigned\n[e1000e] cannot map BAR0\n[e1000e] netbuf exhausted\n[e1000e] out of memory\n[e1000e] implausible BAR0 size\n[e1000e] TX failed\n[e1000e] RX timeout')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-ahci: $(IMG) fat-image sfs-image ahci-image
	TIMEOUT_S=90 QEMU_AHCI_IMG=$(AHCI_IMG) \
	EXTRA_SENTINEL="$$(printf '[ahci] port disk, sectors=16384')" \
	FORBIDDEN_SENTINEL="$$(printf '[ahci] IDENTIFY failed\n[ahci] port would not stop\n[ahci] cannot map ABAR')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# ADR-038 demand-paged-stack gate. THREE arms in one boot (the probe runs them
# in order; arm 3 is fatal by design so it is last):
#   arm 1 growth  -> PRADYOS_STACK_GROW_OK      (64 pages faulted in below the
#                                                8-page eager window)
#   arm 2 syscall -> PRADYOS_STACK_SYSCALL_OK   (write(2) from a buffer on a
#                                                DEMAND-mapped page: this is the
#                                                exact shape Option 1 failed
#                                                0/30 on, via ADR-022's
#                                                never-faults validator)
#   arm 3 guard   -> PRADYOS_STACK_GUARD_ARM then the process is KILLED by the
#                    guard page. The kernel must survive: PRADYOS_SFS_OK is
#                    asserted as a liveness witness AFTER the kill, and the
#                    probe's own "guard page was fillable" line is FORBIDDEN.
# A gate asserting only arm 1 would have passed the regression that motivated it.
smoke-stack-demand: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=stackdemand \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_STACK_GROW_OK\nPRADYOS_STACK_SYSCALL_OK\nPRADYOS_STACK_GUARD_ARM')" \
	FORBIDDEN_SENTINEL="$$(printf 'STACKDEMAND FAIL\nstackdemand probe FAILED to load')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-ftruncate: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=ftruncate \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_FTRUNCATE_OK')" \
	FORBIDDEN_SENTINEL="FTRUNC FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-fsrm: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_FSRM_OK')" \
	FORBIDDEN_SENTINEL="FSRM FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-962: sfs_rename, the half of DDR-956 that has never had a gate. DDR-958's
# smoke-rename drives PRISM's `mv`, which reaches only the FAT root, so it says
# nothing about SFS. This probe is spawned SFS-rooted (main.c, probe_enabled).
#
# Three arms, and each one fails against a stub sfs_rename that returns 0:
#   _OK      the destination reads back the source's exact BYTES, and the source
#            stops opening. Asserting only that the destination opens would pass
#            for a rename that made an empty entry and dropped the inode.
#   _ENOENT  renaming an absent path must fail -- the stub-catcher.
#   _LFN     a long source name is retired: the old name stops resolving and the
#            new one holds the payload. SFS has no VFAT chain to corrupt, so what
#            this exercises is the leaf-slot name copy, name_len, and FNV1a32
#            keying of a name that does not fit the 8.3 shape.
#
# Forbidden sentinels per DDR-956 sec.5; they make the gate ineligible for the
# DDR-785 early exit, so it burns its full window and 90 s is budgeted for that.
smoke-rename-sfs: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=rename-sfs \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SFS_RENAME_OK\nPRADYOS_SFS_RENAME_ENOENT\nPRADYOS_SFS_RENAME_LFN')" \
	FORBIDDEN_SENTINEL="$$(printf 'SFS RENAME FAIL\n[vblk] compl wait timeout\n[vblk] slot wait timeout')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-973: FAT32 multi-cluster reads. ADR-024 sec.D5 deferred init-driven
# execve-respawn on the report that "execve of a large musl-C ELF read from FAT32
# corrupts the loaded image", attributed only as "most likely FAT32 multi-cluster
# reads". That attribution was never measured, and the function the backlog names
# (`read_cluster_chain`) does not exist in this repo -- the reader is fat32_read.
# The defect did not reproduce: `run /CMUSL.ELF` (30,488 B = 60 clusters) execve'd
# clean. This gate makes that refutation permanent and byte-exact. DDR-973 sec.3-5.
#
# mkfs.fat -F32 over 64 MiB gives 1 sector per cluster, so /BIGPAT.BIN (65,536 B)
# is 128 clusters -- 8x the reach of /BIG8K.TXT, the deepest chain any existing
# gate reads. Arm A scans every byte against (7n + 3 + 31*(n>>8)) & 0xFF -- see the
# fat-image recipe for why the second term is load-bearing -- arm B re-walks the
# chain from the head 6 more times at cluster-edge offsets, arm C is ADR-024's case.
#
# WHY THE COUNT ASSERTION BELOW. Arm C execve's /CMUSL.ELF, which prints the same
# PRADYOS_MUSL_OK the boot's SFS-loaded copy does (main.c CMUSL.ELF). A plain
# EXTRA_SENTINEL would therefore pass on the boot copy alone and prove nothing
# about arm C. The denominator is 1 (that boot copy), so the gate demands 2 (R17).
smoke-fat32-multicluster: $(IMG) fat-image sfs-image
	@rm -f build/fat32mc_serial.log
	TIMEOUT_S=90 QEMU_PROBES=fat32mc \
	KEEP_SERIAL=1 SERIAL_LOG=build/fat32mc_serial.log \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_FAT32_MC_OK bytes=65536 clusters=128 straddles=6')" \
	FORBIDDEN_SENTINEL="FAT32MC FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)
	@# Arm C: the execve'd copy must be the SECOND occurrence. One means arm C's
	@# execve never landed -- exactly the ADR-024 symptom -- and the gate fails.
	@# grep -c prints "0" AND exits 1 when there is no match, so `|| echo 0` would
	@# make n="0 0" and the -lt test a syntax error. `|| true` keeps grep's own count;
	@# the :- default covers a missing file, where grep prints nothing and exits 2.
	@n=$$(grep -acF 'PRADYOS_MUSL_OK' build/fat32mc_serial.log 2>/dev/null || true); n=$${n:-0}; \
	  if [ "$$n" -lt 2 ]; then \
	    echo "[smoke] FAIL: arm C -- PRADYOS_MUSL_OK appeared $$n time(s), want 2"; \
	    echo "[smoke]   1 = boot's SFS-loaded cmusl only; the FAT32 execve of"; \
	    echo "[smoke]   /CMUSL.ELF (30,488 B, 60 clusters) did not produce a running image."; \
	    grep -anF 'FAT32MC FAIL' build/fat32mc_serial.log | head -5; \
	    exit 1; \
	  fi; \
	  echo "[smoke] fat32-multicluster: 65536 B / 128 clusters verified, 6 straddles, arm C execve OK ($$n/2 MUSL_OK)"

# DDR-958: fat32_rename, driven through PRISM's `mv` builtin.
#
# WHY THE SHELL AND NOT A PROBE ELF. The natural shape is user/renametest.c
# spawned behind probe_enabled, and it was built that way first. It does not
# fit: every embedded probe costs a page-aligned 8 KiB inside kernel.bin, which
# sits 3,714 B under the 1 MiB stage-2 read window (Makefile:589, DDR-733 as
# raised by DDR-827). Adding it produced a 1,053,054 B kernel — 4,478 B over.
# Raising the window is a boot-path change needing its own DDR, so the gate
# goes through the shell instead, which costs zero image bytes AND tests the
# user-visible feature: DDR-956 shipped `mv` non-functional because fat32 had
# no rename op, and this is the gate that proves it works.
#
# Only the FAT root is covered here, because PRISM runs on the default (FAT)
# mount. sfs_rename stays where DDR-956 left it -- implemented, ungated -- and
# gating it needs the probe ELF, i.e. the boot window above. DDR-958 sec.8.
#
# Structured exactly like smoke-shell: one FIFO feeds the guest UART once
# PRISM_READY appears, then every assertion greps the captured serial log.
# Everything from `@SHIN=` to `) & \` is ONE shell command -- keep make
# comments out of the continuation block (see the note above smoke-shell).
smoke-rename: $(IMG) fat-image sfs-image
	@echo "[rename] booting PRISM; exercising mv on the FAT root..."
	@SHIN=$$(mktemp -u /tmp/pradyos_rename.XXXXXX); rm -f build/rename_serial.log; mkfifo "$$SHIN"; \
	( exec > "$$SHIN"; \
	  for i in $$(seq 1 300); do grep -q PRISM_READY build/rename_serial.log 2>/dev/null && break; sleep 0.1; done; \
	  printf 'echo rename-payload-3v7 > /RENSRC.TXT\n'; sleep 0.6; \
	  printf 'mv /RENSRC.TXT /RENDST.TXT\n'; sleep 0.8; \
	  printf 'cat /RENDST.TXT\n'; sleep 0.6; \
	  printf 'cat /RENSRC.TXT\n'; sleep 0.6; \
	  printf 'mv /NOSUCH9z.TXT /RENX.TXT\n'; sleep 0.8; \
	  printf 'echo dst-original-8k4 > /RENOVR.TXT\n'; sleep 0.6; \
	  printf 'echo src-payload-8k4 > /RENSRC2.TXT\n'; sleep 0.6; \
	  printf 'mv /RENSRC2.TXT /RENOVR.TXT\n'; sleep 0.8; \
	  printf 'echo MARK-OVR-8k4\n'; sleep 0.5; \
	  printf 'cat /RENOVR.TXT\n'; sleep 0.6; \
	  printf 'mv /LongFileName.txt /RENLFN.TXT\n'; sleep 0.8; \
	  printf 'echo MARK-LFN-6r2\n'; sleep 0.5; \
	  printf 'cat /RENLFN.TXT\n'; sleep 0.6; \
	  printf 'cat /LongFileName.txt\n'; sleep 0.6; \
	  printf 'exit\n'; sleep 0.5 ) & \
	timeout 90 qemu-system-x86_64 -M q35 \
	    -drive if=none,format=raw,file=$(IMG),id=disk0 -device virtio-blk-pci,drive=disk0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=disk1 -device virtio-blk-pci,drive=disk1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=disk2 -device virtio-blk-pci,drive=disk2 \
	    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	    -serial stdio -display none -monitor none < "$$SHIN" > build/rename_serial.log 2>/dev/null || true; \
	rm -f "$$SHIN"
	@grep -q "PRISM_READY" build/rename_serial.log || { echo "[rename] FAIL: no PRISM_READY"; tail -30 build/rename_serial.log; exit 1; }
	@# 1. The rename itself. PRISM prints this line only when SYS_RENAME
	@#    returned 0, so a -ENOSYS fat32 (the DDR-956 state) fails here first.
	@grep -q 'mv: /RENSRC.TXT -> /RENDST.TXT' build/rename_serial.log || { echo "[rename] FAIL: mv did not report success (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@# 2. The DATA followed the name. Asserting only that the destination opens
	@#    would pass for a rename that created an empty entry and dropped the
	@#    cluster chain -- the payload is what proves first_clus/size carried.
	@grep -q 'rename-payload-3v7' build/rename_serial.log || { echo "[rename] FAIL: destination does not hold the source's bytes (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@# 3. The source is gone. A copy-style implementation reaches 1 and 2 and
	@#    fails here.
	@grep -q 'cat: cannot open /RENSRC.TXT' build/rename_serial.log || { echo "[rename] FAIL: source still readable after mv (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@# 4. Stub-catcher: renaming an absent path must FAIL. A handler returning 0
	@#    unconditionally passes 1-3 and cannot pass this.
	@grep -q 'mv: cannot rename /NOSUCH9z.TXT' build/rename_serial.log || { echo "[rename] FAIL: mv of an absent path did not fail (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@# 5. An existing destination is REPLACED, not refused and not appended to
	@#    (DDR-958 sec.4, matching sfs_rename). Positional: the marker is printed
	@#    after the mv, so only the lines below it can satisfy this.
	@sed -n '/MARK-OVR-8k4/,$$p' build/rename_serial.log | grep -q 'src-payload-8k4' || { echo "[rename] FAIL: overwritten destination does not hold the source's bytes (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@sed -n '/MARK-OVR-8k4/,$$p' build/rename_serial.log | grep -q 'dst-original-8k4' && { echo "[rename] FAIL: overwritten destination still holds its old bytes (DDR-958)"; tail -40 build/rename_serial.log; exit 1; } || true
	@# 6. THE LONG-NAME ARM -- the reason DDR-958 exists. /LongFileName.txt is
	@#    the FAT image's only VFAT long-named file (fat-image target). Renaming
	@#    it by overwriting the 8.3 name in place leaves the LFN fragments
	@#    spelling the OLD name, and dir_scan prefers the long name -- so the
	@#    file keeps answering to the name it was renamed away from. Arms 1-5
	@#    all pass under that bug because they use short names only.
	@sed -n '/MARK-LFN-6r2/,$$p' build/rename_serial.log | grep -q 'long name read works' || { echo "[rename] FAIL: long-named file unreadable under its new name (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@sed -n '/MARK-LFN-6r2/,$$p' build/rename_serial.log | grep -q 'cat: cannot open /LongFileName.txt' || { echo "[rename] FAIL: the OLD long name still resolves after mv (DDR-958)"; tail -40 build/rename_serial.log; exit 1; }
	@# 7. No fault attributable to this run. fat32_rename walks and rewrites
	@#    directory sectors; a bad slot offset shows up as a fault, not a wrong
	@#    string, and every assertion above would still pass.
	@#    SCOPED, deliberately: two boot probes fault ON PURPOSE every boot --
	@#    WXVIOL.ELF writes to its own text to prove W^X, and METRIC.ELF writes
	@#    the read-only metric page. A blanket '#PF' grep fails on those and
	@#    says nothing about rename. So: no kernel-level [BUG]/PANIC anywhere,
	@#    and no user trap at all once PRISM is up, which is where every mv runs.
	@! grep -qE '\[BUG\]|PANIC' build/rename_serial.log || { echo "[rename] FAIL: kernel BUG/PANIC during the rename run"; tail -40 build/rename_serial.log; exit 1; }
	@sed -n '/PRISM_READY/,$$p' build/rename_serial.log | grep -q '\[trap\]' && { echo "[rename] FAIL: user fault after PRISM came up (DDR-958)"; tail -40 build/rename_serial.log; exit 1; } || true
	@echo "[rename] PASS — fat32_rename: move, replace, absent-path refusal, long-name retirement"

# DDR-748 system-introspection gate: the SYS_SYSINFO probe reads CPU vendor/brand,
# feature bits, CPU count, uptime, and free-frame count, validates them, and prints
# PRADYOS_SYSINFO_OK. Deterministic on QEMU (stable CPUID + frame count).
smoke-sysinfo: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SYSINFO_OK')" \
	FORBIDDEN_SENTINEL="SYSINFO FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-749 wall-clock gate: the SYS_TIME probe reads the broken-down RTC time,
# prints it, and range-validates each field (exact value is host-provided, ranges
# always hold -> deterministic). PRADYOS_TIME_OK on all-pass.
smoke-time: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_TIME_OK')" \
	FORBIDDEN_SENTINEL="TIME FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-750 kernel-log gate: the SYS_DMESG probe writes a unique marker (captured
# via kputc into the log ring), reads the log back through the syscall, and
# confirms the marker is present -> PRADYOS_DMESG_OK. Ring-size-independent.
smoke-dmesg: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_DMESG_OK')" \
	FORBIDDEN_SENTINEL="DMESG FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-755 process-kill gate: the probe forks a child that spins in ring 3 forever,
# SIGKILLs it, and reaps it. PRADYOS_KILL_OK prints only if the kill+reap path
# works (the child has no exit path, so a broken kill would block wait4 -> clean
# timeout, never a false pass). Allow extra wall time for TCG signal delivery.
smoke-kill: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf 'PRADYOS_KILL_OK')" \
	FORBIDDEN_SENTINEL="KILL FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-756 self-rename gate: the probe SYS_SETNAMEs itself to "KILROY", then walks
# SYS_GETPROCS to confirm its own process-table entry shows the new name.
smoke-setname: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SETNAME_OK')" \
	FORBIDDEN_SENTINEL="SETNAME FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5a user gate: the kernel formats a blank SFS volume (disk2), writes the
# embedded static ELFs to it, then loads each BACK FROM SFS into a fresh
# per-process address space with W^X enforced and enters ring 3.
#   - hello prints its banner and exits cleanly via sys_exit (happy path);
#   - wxviol writes to its own RX text page -> #PF -> the kernel kills the process
#     cleanly and keeps running (W^X negative regression).
# Asserts: the loader line, the ring-3 banner, the clean exit, the user-kill
# trap line, AND a post-kill SFS self-test line (proves the kernel survived the
# fault) — full ELF-loader + W^X enforcement end-to-end. Two 8 MiB-stack loads
# push past the default 30 s, so allow more wall time.
smoke-user: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf '[user] ELF loaded from SFS; ring-3 thread spawned\nHELLO FROM RING-3\n[user] sys_exit(0)\n[trap] user #PF page fault\n[sfs] lz4+tags compress/readback/tag OK\nPRADYOS_TLS_OK WRITEV_OK\nPRADYOS_MUSL_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# 5d FPU-context-switch gate (ADR-023 §D8): two concurrent ring-3 processes pin
# their unique pid into XMM0, yield to interleave, and require XMM0 to survive.
# Both print PRADYOS_FPU_OK only if the scheduler saves/restores FPU state per
# thread; without it they clobber each other and print FPU_FAIL (so OK is absent
# and the gate fails). Same image as smoke-user — the test runs at boot.
smoke-fpu: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL="PRADYOS_FPU_OK" FORBIDDEN_SENTINEL="PRADYOS_FPU_FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# 5d pradyos-init (PID 1) gate: init prints its banner, forks a child that exits
# 42, and reaps it via waitpid(-1, WNOHANG) — proving the reap path collects an
# exited child (no leaked zombie). Greps the banner AND the reap line with the
# child's exit code.
smoke-init: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'PRADYOS_INIT_OK\ninit: reaped PID=\n[svc] start exectest pid=\n[svc] exit exectest\n[svc] refuse agentsvc\n[svc] giveup missing after 3 restarts')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# 5e PRISM shell gate (ADR-024 §D6): boot with -serial stdio, pipe a command
# script (echo / help / exit) into the guest UART, and assert the shell came up
# and ran the builtins with no kernel panic. Self-contained (boot_test.sh is
# output-only). FAT32 carries /PRISM.ELF (init execve's it); SFS carries init.
# DDR-916/TASK3: the QEMU bound below was `timeout 60`, which is LESS than this
# gate's own measured duration — tools/ci/gate_shards.txt records smoke-shell at
# 61 s. Derived floor: the feeder's sleeps total 31.4 s AFTER it waits for
# PRISM_READY, which lands ~30 s into boot => ~62 s required. 120 s is ~2x that
# measured floor, not a guess.
#
# DDR-881 job control: the feeder backgrounds TWO copies of /EXECTEST.ELF.
# `fg %1` foregrounds and reaps job [1], which is the fg-path coverage. Job [2]
# is never foregrounded, so it finishes on its own and jobs_reap()
# (user/prism.c:251, called at the prompt from :313) reports it as
# "Done(0)   /EXECTEST.ELF" — the string this gate asserts at the grep below.
# With only ONE job the gate could never pass: `fg` consumes the job
# synchronously, so there is nothing left for jobs_reap to announce, which is
# correct shell behaviour (bash does not print Done for a job you foregrounded).
# The sleep after the second `jobs` is 1.2 s so job [2] has exited by then.
#
# NOTE: everything from the `@SHIN=...` line to `) & \` is ONE shell command
# joined by backslash continuations. A `@#` make-comment inserted inside that
# run is spliced into the shell text and breaks its quoting
# ("/bin/sh: Syntax error: Unterminated quoted string"), which makes the recipe
# die BEFORE it boots QEMU while build/shell_serial.log keeps its previous
# contents — so the gate then greps a stale log and misreports. Keep commentary
# out of the continuation block.
smoke-shell: $(IMG) fat-image sfs-image
	@echo "[shell] booting PRISM; feeding commands once the prompt appears..."
	@# The FIFO must live on a real Linux fs — DrvFs (/mnt/c, where build/ is) does
	@# not support FIFOs. The serial log stays in build/.
	@SHIN=$$(mktemp -u /tmp/pradyos_prism.XXXXXX); rm -f build/shell_serial.log; mkfifo "$$SHIN"; \
	( exec > "$$SHIN"; \
	  for i in $$(seq 1 300); do grep -q PRISM_READY build/shell_serial.log 2>/dev/null && break; sleep 0.1; done; \
	  printf 'echo prism-echo-marker\n'; sleep 0.5; printf 'help\n'; sleep 0.5; printf 'ls /\n'; sleep 0.5; printf 'ps\n'; sleep 0.5; \
	  printf 'touch /PRISMNEW.TXT\n'; sleep 0.5; printf 'ls /\n'; sleep 0.5; printf 'rm /PRISMNEW.TXT\n'; sleep 0.5; \
	  printf 'uname\n'; sleep 0.5; printf 'date\n'; sleep 0.5; printf 'uptime\n'; sleep 0.5; printf 'dmesg\n'; sleep 0.5; printf 'free\n'; sleep 0.5; \
	  printf 'echo redir-ok-7q2 > /REDIR.TXT\n'; sleep 0.5; printf 'cat /REDIR.TXT\n'; sleep 0.5; printf 'ls /\n'; sleep 0.5; \
	  printf 'echo pipe-marker-4k8 | cat\n'; sleep 0.7; printf 'ls / | cat\n'; sleep 0.7; \
	  printf 'echo aaa-8w1 > /APP.TXT\n'; sleep 0.5; printf 'echo bbb-8w1 >> /APP.TXT\n'; sleep 0.5; printf 'cat /APP.TXT\n'; sleep 0.5; \
	  printf 'echo in-marker-8w1 > /IN.TXT\n'; sleep 0.5; printf 'cat < /IN.TXT\n'; sleep 0.5; \
	  printf 'echo longrec-9x3-aaaaaaaaaaaaaaaaaa-TAIL9x3 > /TR.TXT\n'; sleep 0.5; \
	  printf 'echo short-9x3 > /TR.TXT\n'; sleep 0.5; printf 'cat /TR.TXT\n'; sleep 0.5; \
	  printf 'echo st-ok=$$?\n'; sleep 0.6; \
	  printf 'run /NOPE789.ELF\n'; sleep 0.9; printf 'echo st-fail=$$?\n'; sleep 0.6; \
	  printf 'cat /BIG8K.TXT | cat\n'; sleep 3.5; \
	  printf 'echo pipe3-m7q | cat | cat\n'; sleep 0.9; \
	  printf 'cat /NOPE9k2.TXT > /OUT9k2.TXT 2> /ERR9k2.TXT\n'; sleep 0.7; \
	  printf 'cat /ERR9k2.TXT\n'; sleep 0.5; \
	  printf 'cat /NOPE55a.TXT 2>> /EAP55a.TXT\n'; sleep 0.7; \
	  printf 'cat /NOPE55b.TXT 2>> /EAP55a.TXT\n'; sleep 0.7; \
	  printf 'cat /EAP55a.TXT\n'; sleep 0.6; \
	  printf 'cat /NOPE66c.TXT > /BOTH66c.TXT 2>&1\n'; sleep 0.7; \
	  printf 'echo MARKER66c\n'; sleep 0.5; \
	  printf 'cat /BOTH66c.TXT\n'; sleep 0.6; \
	  printf 'agent list\n'; sleep 0.6; \
	  printf 'agent spawn /NOPE.ELF probe\n'; sleep 0.6; \
	  printf 'action submit 4 hello\n'; sleep 0.6; \
	  printf 'action approve 1\n'; sleep 0.6; \
	  printf 'run /EXECTEST.ELF &\n'; sleep 1.2; \
	  printf 'run /EXECTEST.ELF &\n'; sleep 1.2; \
	  printf 'jobs\n'; sleep 0.6; \
	  printf 'fg %%1\n'; sleep 1.5; \
	  printf 'jobs\n'; sleep 1.2; \
	  printf 'kill %%99\n'; sleep 0.6; \
	  printf 'exit\n'; sleep 0.5 ) & \
	timeout 120 qemu-system-x86_64 -M q35 \
	    -drive if=none,format=raw,file=$(IMG),id=disk0 -device virtio-blk-pci,drive=disk0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=disk1 -device virtio-blk-pci,drive=disk1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=disk2 -device virtio-blk-pci,drive=disk2 \
	    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	    -serial stdio -display none -monitor none < "$$SHIN" > build/shell_serial.log 2>/dev/null || true; \
	rm -f "$$SHIN"
	@# DDR-868 (item 33): `2>>` APPENDS. Two failing commands both redirect to
	@# one file; the second must not truncate the first. A `2>>` wired to
	@# O_TRUNC would leave exactly one line and still look like it worked.
	@# BOTH names must survive in the file. If `2>>` were wired to O_TRUNC the
	@# second command would overwrite the first, leaving only NOPE55b — which a
	@# mere "the file is non-empty" check would happily accept.
	@grep -q 'cat: cannot open /NOPE55a.TXT' build/shell_serial.log || { echo "[shell] FAIL: 2>> truncated the earlier entry (DDR-868)"; tail -40 build/shell_serial.log; exit 1; }
	@grep -q 'cat: cannot open /NOPE55b.TXT' build/shell_serial.log || { echo "[shell] FAIL: 2>> lost the later entry (DDR-868)"; tail -40 build/shell_serial.log; exit 1; }
	@# DDR-888 (item 36): the agent DSL. PRISM holds neither CAP_AGENT nor
	@# CAP_SOVEREIGN, so the privileged verbs MUST be refused — the refusal
	@# printing IS the assertion. A shell that offered `agent spawn` and did
	@# nothing would look like a missing feature; one that appeared to succeed
	@# would be a capability hole.
	@grep -q 'AGENT ROSTER slots=' build/shell_serial.log || { echo "[shell] FAIL: agent list did not read the roster (DDR-888)"; tail -40 build/shell_serial.log; exit 1; }
	@grep -q 'AGENT SPAWN DENIED' build/shell_serial.log || { echo "[shell] FAIL: agent spawn NOT denied without CAP_AGENT (DDR-888)"; tail -40 build/shell_serial.log; exit 1; }
	@grep -q 'ACTION SUBMIT DENIED' build/shell_serial.log || { echo "[shell] FAIL: action submit NOT denied for a non-agent (DDR-888)"; tail -40 build/shell_serial.log; exit 1; }
	@grep -q 'ACTION APPROVE DENIED' build/shell_serial.log || { echo "[shell] FAIL: action approve NOT denied without CAP_SOVEREIGN (DDR-888)"; tail -40 build/shell_serial.log; exit 1; }
	@# DDR-881 (item 34): job control. Three DETERMINISTIC facts only —
	@# `jobs` and `fg` output races the child's exit (HELLO.ELF finishes in
	@# milliseconds), so asserting on them would be a flaky gate dressed up as
	@# a feature test.
	@# 1. '[1] <pid>' proves `&` was parsed and the shell forked WITHOUT waiting.
	@grep -qE '\[1\] [0-9]+' build/shell_serial.log || { echo "[shell] FAIL: '&' did not background (DDR-881)"; tail -40 build/shell_serial.log; exit 1; }
	@# 2. 'Done' proves the non-blocking reap ran and reported. Without it the
	@#    child stays a zombie holding its TCB and nothing ever collects it.
	@grep -q 'Done(0)   /EXECTEST.ELF' build/shell_serial.log || { echo "[shell] FAIL: background job never reaped (DDR-881)"; tail -40 build/shell_serial.log; exit 1; }
	@# 3. `kill %99` must REFUSE. A missing job left pid at 0, and kill(0,sig) is
	@#    a process-group broadcast, not a no-op — so falling through here would
	@#    signal everything rather than doing nothing.
	@grep -q 'kill: no such job %99' build/shell_serial.log || { echo "[shell] FAIL: kill %n did not reject an unknown job (DDR-881)"; tail -40 build/shell_serial.log; exit 1; }
	@# The message above prints BEFORE the guard is consulted, so it passes even
	@# if the code then falls through with pid 0 — mutation testing caught that.
	@# This second check tests the BEHAVIOUR: no kill(2) call must be attempted
	@# at all. A fall-through calls kill(0,...) and prints 'kill: pid 0 not
	@# found'; the correct path prints nothing further.
	@#
	@# NOTE, so the guard is not oversold: kill(0) is NOT a process-group
	@# broadcast here — PRADYOS has no process groups, so the kernel simply finds
	@# no pid 0 and returns an error. The guard is defence in depth against the
	@# day pid semantics grow, not a fix for a live escalation.
	@! grep -q 'kill: pid 0 not found' build/shell_serial.log || { echo "[shell] FAIL: kill %n fell through to pid 0 (DDR-881)"; tail -40 build/shell_serial.log; exit 1; }
	@# DDR-868: `2>&1` AFTER `>` sends stderr to the SAME file as stdout. If it
	@# were applied before the stdout swap it would capture the console instead,
	@# the error would print to the terminal, and /BOTH66c.TXT would be empty —
	@# which is the bug this syntax exists to prevent.
	@# POSITION is the discriminator, not presence. Grepping the whole log for
	@# the error matches whether it landed in the FILE or on the CONSOLE, so it
	@# cannot tell a working 2>&1 from one applied before the stdout swap — a
	@# mutation proved exactly that assertion useless. The marker is printed
	@# AFTER the redirect and BEFORE the cat, so the error text may only appear
	@# in the lines that FOLLOW it.
	@sed -n '/MARKER66c/,$$p' build/shell_serial.log | grep -q 'cat: cannot open /NOPE66c.TXT' || { echo "[shell] FAIL: 2>&1 did not send stderr to the stdout file (DDR-868)"; tail -40 build/shell_serial.log; exit 1; }
	@# ...and it must NOT have reached the console before the marker.
	@sed -n '1,/MARKER66c/p' build/shell_serial.log | grep -q 'cat: cannot open /NOPE66c.TXT' && { echo "[shell] FAIL: 2>&1 leaked stderr to the console (DDR-868)"; tail -40 build/shell_serial.log; exit 1; } || true
	@grep -q "PRISM_READY"            build/shell_serial.log || { echo "[shell] FAIL: no PRISM_READY";  tail -30 build/shell_serial.log; exit 1; }
	@grep -q "prism> "               build/shell_serial.log || { echo "[shell] FAIL: no prism> prompt"; exit 1; }
	@grep -q "prism-echo-marker"     build/shell_serial.log || { echo "[shell] FAIL: echo builtin";     exit 1; }
	@grep -q "builtins: help echo"   build/shell_serial.log || { echo "[shell] FAIL: help builtin";     exit 1; }
	@# DDR-742: PRISM's `ls /` prints bare names, distinct from the kernel's boot
	@# fs_list ("    HELLO.TXT  25 bytes") and "[fs] /HELLO.TXT:". The prompt
	@# "prism> " may share the first output line (flush/read timing — flaky if
	@# anchored to BOL, DDR-743), so accept BOL *or* a "prism> " prefix while the
	@# trailing "$$" still excludes the kernel's " 25 bytes"/":" suffixed lines.
	@grep -qE "(^|prism> )HELLO\.TXT$$" build/shell_serial.log || { echo "[shell] FAIL: ls builtin (DDR-742)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-743: PRISM's `ps` prints a "  PID  PPID S U NAME" header (emitted by no
	@# other path) followed by the ring listing — proves SYS_GETPROCS end-to-end.
	@# (the prompt "prism> " may share the header's line, so don't anchor to BOL)
	@# DDR-754: header now carries the CPUms + DISP accounting columns.
	@grep -qE "PID +PPID S U +CPUms +DISP NAME$$" build/shell_serial.log || { echo "[shell] FAIL: ps builtin (DDR-743/754)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-745: `touch /PRISMNEW.TXT` then `ls /` must list the created file (proves
	@# O_CREAT through the shell on the real FAT root), and `rm` must confirm removal.
	@grep -qE "(^|prism> )PRISMNEW\.TXT$$" build/shell_serial.log || { echo "[shell] FAIL: touch builtin (DDR-745)"; tail -30 build/shell_serial.log; exit 1; }
	@grep -qF "rm: removed /PRISMNEW.TXT" build/shell_serial.log || { echo "[shell] FAIL: rm builtin (DDR-745)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-751: system-introspection builtins. Values vary but the shapes are fixed.
	@grep -qE "uname: .*cpus="        build/shell_serial.log || { echo "[shell] FAIL: uname builtin (DDR-751)";  tail -30 build/shell_serial.log; exit 1; }
	@grep -qE "date: 20[0-9][0-9]-"   build/shell_serial.log || { echo "[shell] FAIL: date builtin (DDR-751)";   tail -30 build/shell_serial.log; exit 1; }
	@grep -qE "uptime: [0-9]+s"       build/shell_serial.log || { echo "[shell] FAIL: uptime builtin (DDR-751)"; tail -30 build/shell_serial.log; exit 1; }
	@grep -qE "dmesg: [1-9][0-9]* bytes" build/shell_serial.log || { echo "[shell] FAIL: dmesg builtin (DDR-751)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-752: `free` prints total/free/used physical memory (KiB). Shape is fixed.
	@grep -qE "mem: total=[0-9]+K free=[0-9]+K used=[0-9]+K" build/shell_serial.log || { echo "[shell] FAIL: free builtin (DDR-752)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-778: output redirection. BOTH assertions are required and neither alone
	@# is sufficient: the marker on its own would still appear if redirection did
	@# nothing (a plain `echo` prints it to the console), so the file must ALSO show
	@# up in `ls /` to prove it was actually created and written.
	@grep -qE "(^|prism> )REDIR\.TXT$$" build/shell_serial.log || { echo "[shell] FAIL: redirect did not create the file (DDR-778)"; tail -30 build/shell_serial.log; exit 1; }
	@grep -qF "redir-ok-7q2" build/shell_serial.log || { echo "[shell] FAIL: redirect content not read back (DDR-778)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-780: pipes. The marker must arrive THROUGH the pipe (echo -> cat's fd 0).
	@# Discriminating: if `|` were ignored, tokenize would hand echo the literal
	@# tokens and it would print "pipe-marker-4k8 | cat". So the marker must be
	@# present AND must never appear with the trailing pipe tokens. (Asserting
	@# HELLO.TXT from `ls / | cat` would NOT discriminate — the plain `ls /` earlier
	@# in this same session already prints it.)
	@grep -qF "pipe-marker-4k8" build/shell_serial.log || { echo "[shell] FAIL: pipe delivered no output (DDR-780)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qF "pipe-marker-4k8 | cat" build/shell_serial.log; then echo "[shell] FAIL: '|' not honoured — echo printed the pipe tokens (DDR-780)"; tail -30 build/shell_serial.log; exit 1; fi
	@# DDR-781/782: append `>>` must PRESERVE the earlier record. Discriminating: if
	@# `>>` silently behaved like `>`, the second write would replace the file (since
	@# DDR-782, `>` truncates) and aaa-8w1 would be gone. Requiring BOTH records is
	@# what proves O_APPEND positions at end-of-file.
	@grep -qF "aaa-8w1" build/shell_serial.log || { echo "[shell] FAIL: '>>' overwrote instead of appending (DDR-781)"; tail -30 build/shell_serial.log; exit 1; }
	@grep -qF "bbb-8w1" build/shell_serial.log || { echo "[shell] FAIL: '>>' second record missing (DDR-781)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-781: input redirection. If `<` were ignored, cat would take "<" as its
	@# path and print exactly "cat: cannot open <" — so forbid that AND require the
	@# marker that can only arrive through fd 0.
	@grep -qF "in-marker-8w1" build/shell_serial.log || { echo "[shell] FAIL: '<' delivered no stdin (DDR-781)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qF "cat: cannot open <" build/shell_serial.log; then echo "[shell] FAIL: '<' not honoured — cat got '<' as a path (DDR-781)"; tail -30 build/shell_serial.log; exit 1; fi
	@# DDR-782: `>` must TRUNCATE. /TR.TXT is written long, then short; `cat` must
	@# show the short record and NOT the long record's tail. Discriminating by
	@# construction: without kernel O_TRUNC the short write lands at offset 0 and
	@# leaves "...-TAIL9x3" behind, which is exactly the pre-DDR-782 behaviour, so
	@# this assertion FAILS before the fix and passes after. TAIL9x3 reaches the log
	@# only via `cat` — the long line went to the file, never to the console.
	@grep -qF "short-9x3" build/shell_serial.log || { echo "[shell] FAIL: truncating write not read back (DDR-782)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qF "TAIL9x3" build/shell_serial.log; then echo "[shell] FAIL: '>' did not truncate — stale tail survived (DDR-782)"; tail -30 build/shell_serial.log; exit 1; fi
	@# DDR-789: `$$?` expands to the last exit status. Discriminating by
	@# construction: before this slice the tokenizer passed `$$?` through untouched
	@# and echo printed it LITERALLY, so forbidding "st-ok=$$?" fails deterministically
	@# on the old behaviour. 127 is what do_run's child exits when execve fails, so
	@# the failure value is asserted too, not just that expansion happened.
	@grep -qaF "st-ok=0" build/shell_serial.log || { echo "[shell] FAIL: \$$? did not expand to 0 after a successful command (DDR-789)"; tail -30 build/shell_serial.log; exit 1; }
	@grep -qaF "st-fail=127" build/shell_serial.log || { echo "[shell] FAIL: \$$? did not report 127 after a failed run (DDR-789)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qaF 'st-ok=$$?' build/shell_serial.log; then echo "[shell] FAIL: '\$$?' printed literally — no expansion (DDR-789)"; tail -30 build/shell_serial.log; exit 1; fi
	@# DDR-787: >4 KiB must survive a pipe. /BIG8K.TXT is ~7.8 KiB (200 payload
	@# lines); PIPE_SIZE is 4096, so the pre-DDR-787 non-blocking write truncated at
	@# ~107 lines and silently dropped the rest. Requiring >=180 is far above that
	@# ceiling, so it fails before the fix and passes after.
	@# Counted rather than matched exactly, deliberately: concurrent kernel prints
	@# interleave on the same COM1 and can split an individual line mid-string (a
	@# measured artefact — `pipe p[sfs] journal ... OK` then `ayload line 099`), so
	@# an exact 200/200 assertion would flake on console interleaving, not on pipe
	@# behaviour. The threshold keeps a wide margin on both sides.
	@n=$$(grep -ac "pipe payload line" build/shell_serial.log); \
	  if [ "$$n" -lt 180 ]; then echo "[shell] FAIL: only $$n/200 payload lines survived the pipe — truncated at PIPE_SIZE? (DDR-787)"; tail -30 build/shell_serial.log; exit 1; fi
	@grep -qaF "BIGHEAD-e5v" build/shell_serial.log || { echo "[shell] FAIL: big-pipe transfer never started (DDR-787)"; tail -30 build/shell_serial.log; exit 1; }
	@# DDR-786: multi-stage pipelines. The marker must traverse TWO pipes, and the
	@# second `|` must never reach a builtin as an argument. Discriminating: before
	@# DDR-786 only the FIRST `|` was honoured, so `cat` received "| cat" and printed
	@# exactly "cat: cannot open |" — forbidding that string fails deterministically
	@# on the old behaviour. The marker alone would NOT discriminate, since a single
	@# working pipe already prints it.
	@grep -qF "pipe3-m7q" build/shell_serial.log || { echo "[shell] FAIL: 3-stage pipeline delivered no output (DDR-786)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qF "cat: cannot open |" build/shell_serial.log; then echo "[shell] FAIL: second '|' reached the builtin as an argument (DDR-786)"; tail -30 build/shell_serial.log; exit 1; fi
	@# DDR-784: stderr + `2>`. The command redirects stdout and stderr to DIFFERENT
	@# files, which is what makes this discriminate. If `2>` works, cat's error goes
	@# to /ERR9k2.TXT and only reaches the console when we cat that file. If it does
	@# NOT work, the error goes to fd 1 -> /OUT9k2.TXT, never reaches the console,
	@# and /ERR9k2.TXT is empty — so the marker is ABSENT. Asserting presence after
	@# `cat /ERR9k2.TXT` therefore fails before this change and passes after, and
	@# proves the message travelled on fd 2 (fd 1 pointed elsewhere in that command).
	@grep -qF "cat: cannot open /NOPE9k2.TXT" build/shell_serial.log || { echo "[shell] FAIL: stderr not redirected to the 2> file (DDR-784)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/shell_serial.log; then echo "[shell] FAIL: kernel panic"; tail -30 build/shell_serial.log; exit 1; fi
	@echo "[shell] PASS — PRISM_READY + prompt + echo + help + ls + ps + touch/rm + uname/date/uptime/dmesg/free + redirect(> >> < 2>) + truncate/append + stderr + pipes(N-stage, >4KiB), clean, no panic."

# Phase 5b slice 2 user-access gate: the in-kernel uaccess self-test (main.c)
# drives copyin/copyout/copyinstr against a throwaway user AS — a good page, a
# wild pointer (-> EFAULT), a read-only page write (-> EFAULT, W^X), and a valid
# string. All four lines must appear AND the kernel must survive the two faults.
smoke-uaccess: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf '[uaccess] copyin good page OK\n[uaccess] copyin bad ptr EFAULT OK\n[uaccess] copyout RO page EFAULT OK\n[uaccess] copyinstr OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-757 kernel-self W^X gate: after vmm_protect_kernel re-stamps the kernel
# image mapping (text RX, rodata R+NX, data/BSS RW+NX) it re-walks the PT and
# audits that no text PTE is writable and no non-text PTE is executable, printing
# [wx] kernel W^X OK. The whole boot (incl. SMP gates elsewhere) runs against the
# hardened tables, so this doubles as a no-regression witness.
smoke-wxkernel: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf '[wx] kernel W^X OK')" \
	FORBIDDEN_SENTINEL="kernel W^X FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-758 syscall-fuzz gate: a ring-3 probe floods 3000 hostile syscalls (bad NSI
# numbers -> -ENOSYS via the dispatch bounds check; wild pointers into read-only
# syscalls -> -EFAULT). The kernel must survive every one; PRADYOS_FUZZ_OK prints
# only after all calls return, and any kernel fault is a panic boot_test catches.
smoke-syscallfuzz: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'PRADYOS_FUZZ_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 3 syscall I/O gate: the ring-3 systest program (loaded from SFS)
# calls sys_write(1,...) (good), sys_write(badfd,...) -> EBADF, and
# sys_write(1, NULL, ...) -> EFAULT, printing a sentinel per outcome. Proves the
# fd table + the EFAULT copy path from ring 3, with the kernel surviving. The
# boot now loads three user ELFs, so allow extra wall time.
smoke-sysio: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'SYSWRITE OK\nSYSIO EBADF OK\nSYSIO EFAULT OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 4 file gate: systest opens /HELLO.TXT on the (stable FAT32) root
# mount, fstats it (size > 0), reads its first bytes (ELF/text content), closes
# it, and confirms a missing path -> ENOENT — exercising the fd<->VFS/capability
# bridge and the copyin/copyout path from ring 3.
smoke-sysfile: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'SYSOPEN OK\nSYSFSTAT OK\nSYSREAD OK\nSYSCLOSE OK\nSYSOPEN ENOENT OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 5 process/seek gate: systest calls getpid (>0), getcwd ("/"),
# and lseek-then-read (seek to offset 1, read 'R' from HELLO.TXT).
smoke-sysproc: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'SYSGETPID OK\nSYSGETCWD OK\nSYSLSEEK OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 6 mmap gate: systest mmaps an anon RW+NX page (writes+reads it),
# confirms PROT_EXEC is rejected with EINVAL (W^X), and munmaps then re-mmaps the
# same hint (region freed and reusable).
#
# DDR-877 (item 19) added the two REJECT arms. mmap is the six-argument syscall,
# so it is where the widened ABI is proved: 'FD REJECTED' needs a5 to arrive and
# be read, 'OFF REJECTED' needs a6, and the two expect DIFFERENT errno values, so
# an r8/r9 swap in the marshal fails both instead of passing by symmetry.
smoke-sysmmap: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'SYSMMAP OK\nSYSMMAP WX REJECTED\nSYSMMAP FD REJECTED\nSYSMMAP OFF REJECTED\nSYSMUNMAP OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 7 execve gate: the kernel places /EXECTEST.ELF on the FAT32
# root; the ring-3 systest SYS_EXECVE's it, replacing its own image. The new
# image's sentinel MUST appear, and systest's post-execve "(BUG)" line MUST NOT
# (execve never returns on success). Two ELF loads + an extra AS build push past
# the default wall time. (kmain re-loads systest from SFS first, so the execve
# runs after all the slice 3-6 sentinels.)
smoke-sysexec: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='EXECVE: new image running' \
	    FORBIDDEN_SENTINEL='EXECVE: post-exec (BUG)' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 8 fork gate: the ring-3 systest forks; the child prints its
# sentinel and exits while the parent prints its own and continues. Both lines
# must appear (order is non-deterministic — grepped independently). Two address
# space copies + extra ring-3 runs push past the default wall time.
smoke-sysfork: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'FORK: parent pid=\nFORK: child running pid=')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Phase 5b slice 9 wait4 gate: systest forks a child that exits(42); the parent
# wait4's it, reads the status, and prints it. Proves zombie/reap + status copyout
# (and the orphan reaper keeps zombies from leaking). Two ELF loads + fork/wait.
smoke-syswait: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='WAIT: child exited status=42' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# IMP-A mitigations gate: the kernel probes CPUID and prints the IBRS/STIBP/SSBD/
# IBPB state at boot. On QEMU TCG the values are 0 (no spec-ctrl advertised) — the
# gate only asserts the line is present (the probe ran without faulting).
smoke-mitigations: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='[cpu] mitigations:' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# IMP-B poison gate: with KASAN=1 (the default) the kernel poisons freed PMM
# frames and arms slab canaries. The gate asserts the "[pmm] poison enabled"
# banner; because KASAN is the default, every other gate is implicitly a poison /
# canary regression test too (a smashed canary -> KHEAP PANIC -> missing sentinel).
smoke-pmm-poison: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='[pmm] poison enabled' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# IMP-C vDSO gate: the ring-3 systest reads wall_time_ns from the read-only vDSO
# page (no syscall) and prints it only when non-zero, proving the kernel-updated
# clock is visible in user space. Loads user ELFs, so allow extra wall time.
smoke-vdso: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL='VDSO: clock ns=' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# IMP-D COW fork gate: an in-kernel self-test builds a one-page AS, COW-forks it,
# faults the child's copy, and asserts the parent's page is unchanged (true
# copy-on-write isolation). The ring-3 #PF COW path is additionally covered by
# smoke-sysfork/smoke-syswait (the parent writes its stack after fork).
smoke-cowfork: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='[vmm] COW fork copy-on-write OK' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# NET-A virtio-net gate: boot_test.sh already attaches a virtio-net-pci device.
# The driver brings it up over the virtio transport (negotiate, RX/TX queues,
# MAC, RX armed, DRIVER_OK); the gate asserts the bring-up line. The serial log
# also shows "TX OK" (one frame reaped off the used ring) — true peer loopback
# needs a tap/socket netdev rather than QEMU's SLIRP, deferred to NET-B/later.
# NET-B TCP gate (ADR-025 §D10): QEMU forwards host:18007 -> guest:8007; once the
# stack is up a host TCP client sends PRADYOS_NET_PROBE and the kernel echo server
# returns it (serial: PRADYOS_NET_TCP_OK). Exercises the full RX/TX path through
# virtio-net + lwIP TCP. (/dev/tcp is a bash feature; the recipe shell is dash.)
smoke-net: $(IMG) fat-image sfs-image
	@echo "[net] TCP echo gate: boot + host connects to 127.0.0.1:18007..."
	@rm -f build/net_tcp.log build/net_echo.txt
	@bash -c 'for i in $$(seq 1 400); do grep -q "lwIP up" build/net_tcp.log 2>/dev/null && break; sleep 0.1; done; \
	  sleep 1; \
	  if exec 3<>/dev/tcp/127.0.0.1/18007; then printf "PRADYOS_NET_PROBE\n" >&3; IFS= read -t 8 -r reply <&3; printf "%s\n" "$$reply" > build/net_echo.txt; fi' &
	@timeout 60 qemu-system-x86_64 -M q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -netdev user,id=n0,hostfwd=tcp::18007-:8007 -device virtio-net-pci,netdev=n0 \
	    -serial stdio -display none -monitor none > build/net_tcp.log 2>/dev/null || true
	@grep -q PRADYOS_NET_TCP_OK build/net_tcp.log || { echo "[net] FAIL: no PRADYOS_NET_TCP_OK"; tail -25 build/net_tcp.log; exit 1; }
	@grep -q PRADYOS_NET_PROBE build/net_echo.txt 2>/dev/null || { echo "[net] FAIL: TCP echo not received by host"; exit 1; }
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/net_tcp.log; then echo "[net] FAIL: kernel panic"; exit 1; fi
	@echo "[net] PASS — TCP echo on :8007 (PRADYOS_NET_TCP_OK + host echo)."

# NET-B loopback gate (ADR-025 §D10): the kernel sends a UDP datagram to
# 127.0.0.1:7 through lwIP's loopback interface and its recv callback fires.
smoke-net-lo: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'msix vec=54\n[net] lwIP up 10.0.2.15/24\nPRADYOS_NET_LO_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# NET-B fuzz/hardening gate (ADR-025 §D6/§D10): at boot the kernel feeds 512
# malformed/truncated frames and a 256-segment SYN flood (to a closed port)
# straight into the lwIP receive path. Passing = the kernel survives and prints
# the sentinel; boot_test.sh already fails the run on any panic string.
smoke-net-fuzz: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL=PRADYOS_NET_FUZZ_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-753 TCP loopback gate: the kernel opens a TCP client to the in-guest echo
# server on 127.0.0.1:8007, completes the handshake + echo over the loopback
# netif, and verifies the returned bytes. Deterministic (no external network).
smoke-net-tcp-lo: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL=PRADYOS_NET_TCP_LO_OK \
	FORBIDDEN_SENTINEL=PRADYOS_NET_TCP_LO_FAIL \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER queue gate (ADR-026 §D2/§D3): the in-boot self-test submits an action,
# sovereign mode auto-approves it, and an audit entry is written.
smoke-aether-queue: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL=PRADYOS_AETHER_QUEUE_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER functional gate (ADR-026 §D10/§D11): the daemon (CAP_SOVEREIGN) boots,
# spawns the test agent (CAP_AGENT); the agent submits ACTION_WRITE_FILE which
# sovereign mode auto-approves; the agent executes it and exits. End-to-end:
# queue -> daemon -> agent -> approve -> execute -> done.
smoke-aether: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'PRADYOS_AETHER_QUEUE_OK\nPRADYOS_AETHER_DAEMON_OK\nPRADYOS_AGENT_VERIFIED\nPRADYOS_AGENT_DONE')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 slice 0 GPU gate (ADR-028): boot with a virtio-gpu-pci device; the
# kernel brings up scanout 0 over the 2D control queue (GET_DISPLAY_INFO ->
# CREATE_2D -> ATTACH_BACKING -> SET_SCANOUT -> TRANSFER -> FLUSH) and presents a
# BGRA framebuffer. Headless QEMU still ACKs every command, so the bring-up is
# verifiable via the serial sentinel (like the NET-A virtio-net gate).
smoke-gpu: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_GPU=1 EXTRA_SENTINEL=PRADYOS_GPU_FB_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# NVMe controller bring-up gate (DDR-765): boot with a QEMU -device nvme backed
# by a small raw image; the driver maps BAR0, enables the controller, stands up
# the admin queue, and runs Identify Controller + Namespace, printing the model +
# LBA geometry ("[nvme] <model> ns1 <N> LBAs x <sz> B"). The FORBIDDEN patterns
# catch every bring-up failure path.
build/nvme.img:
	mkdir -p build && truncate -s 16M build/nvme.img

# Cross-reboot persistence proof (DDR-768): mkfs.sfs writes /PERSIST.TXT onto a
# host image; the kernel mounts that image (attached LAST via QEMU_SFS2, never
# reformatted), reads it back, and prints PRADYOS_SFS_PERSIST_OK. Proves the
# kernel decodes a host-authored SFS volume byte-for-byte end-to-end.
SFS_PERSIST_MARK768 := PRADYOS-SFS-PERSIST-DDR768-OK
SFS_NESTED_MARK769  := PRADYOS-SFS-NESTED-DDR769-OK
smoke-sfs-persist: $(IMG) fat-image sfs-image $(MKFS_SFS)
	@mkdir -p build
	printf '%s' '$(SFS_PERSIST_MARK768)' > build/persist768.txt
	printf '%s' '$(SFS_NESTED_MARK769)' > build/nested769.txt
	printf 'pad' > build/pad773.txt
	@:  # DDR-773: 6 extra root files push this image past SFS_LEAF_MAX(14) slots,
	@:  # so the KERNEL below mounts a genuinely multi-leaf host-authored tree.
	$(MKFS_SFS) $(MKFS_SFS_IMG) --blocks 4096 \
	    --file PERSIST.TXT=build/persist768.txt \
	    --file /etc/test/config=build/nested769.txt \
	    --file P0.TXT=build/pad773.txt --file P1.TXT=build/pad773.txt \
	    --file P2.TXT=build/pad773.txt --file P3.TXT=build/pad773.txt \
	    --file P4.TXT=build/pad773.txt --file P5.TXT=build/pad773.txt
	TIMEOUT_S=90 QEMU_SFS2=1 QEMU_NO_EXT4=1 \
	    EXTRA_SENTINEL="$$(printf 'PRADYOS_SFS_PERSIST_OK\nPRADYOS_SFS_NESTED_OK')" \
	    FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_SFS_PERSIST_FAIL\nPRADYOS_SFS_NESTED_FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-770: a host-provisioned SFS ROOT image carrying the real /etc/aether/config
# (nested-dir provisioning, DDR-769). The kernel roots the AETHER daemon here
# instead of formatting+provisioning blk2, so the boot policy ships in the build.
AETHER_CFG_TEXT := mode=sovereign\ntask=test\nslot=0\nnet=10.0.2.2:11434\n
build/sfsroot.img: $(MKFS_SFS)
	@mkdir -p build
	printf '$(AETHER_CFG_TEXT)' > build/aethercfg.txt
	$(MKFS_SFS) build/sfsroot.img --blocks 4096 --file /etc/aether/config=build/aethercfg.txt

# DDR-770/771: five virtio-blk disks — boot(0)/fat(1)/sfs(2)/ext4(3)/sfsroot(4).
# `blk4 ready` proves the 5th disk registered (impossible under the old
# VBLK_MAX=4), and the AETHER daemon roots at the provisioned image at blk4 —
# i.e. the provisioned root coexists with ext4 (no QEMU_NO_EXT4).
smoke-aether-sfsroot: $(IMG) fat-image sfs-image ext4-image build/sfsroot.img
	TIMEOUT_S=90 QEMU_SFSROOT=1 \
	    EXTRA_SENTINEL="$$(printf 'blk4 ready\nrooted at provisioned mkfs image\nPRADYOS_AETHER_CFG_OK mode=sovereign')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# ADR-032: FS write-budget token-bucket. The kernel self-test writes 1.5 MiB
# (> the 1 MiB burst cap) from one thread across per-tick refills → the lifetime
# cap is gone, the rate limit holds. FORBIDDEN catches a non-refilling budget.
smoke-fs-budget: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 \
	    EXTRA_SENTINEL=PRADYOS_FS_BUDGET_OK \
	    FORBIDDEN_SENTINEL=PRADYOS_FS_BUDGET_FAIL \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-nvme: $(IMG) fat-image sfs-image build/nvme.img
	TIMEOUT_S=60 QEMU_NVME=1 \
	    EXTRA_SENTINEL="$$(printf '[nvme] \nLBAs x \nregistered nvme0\n[nvme] msix vec=50\nPRADYOS_NVME_RW_OK\nPRADYOS_NVME_PRP_OK\nPRADYOS_NVME_IRQ_OK')" \
	    FORBIDDEN_SENTINEL="$$(printf 'controller not ready\nidentify-ctrl failed\nidentify-ns failed\nreset stuck\ncreate-iocq failed\ncreate-iosq failed\nmsix unavailable\nPRADYOS_NVME_RW_FAIL\nPRADYOS_NVME_PRP_FAIL\nPRADYOS_NVME_IRQ_FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 framebuffer-surface gate (DDR-702): proves the ring-3 SYS_FB map+draw+
# flush path. Served by the in-house compositor (DDR-704), which is the real FB
# consumer and prints PRADYOS_FB_DRAW_OK on its first frame. Needs the GPU device.
smoke-fb: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL=PRADYOS_FB_DRAW_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 keyboard-input gate (DDR-703): a ring-3 reader announces
# PRADYOS_INPUT_WAIT and polls SYS_INPUT_POLL; the harness then injects real key
# presses via QEMU's HMP monitor (`sendkey`), so the i8042 raises IRQ1 — the
# genuine hardware path. The byte must reach ring 3 (PRADYOS_INPUT_OK a).
smoke-input: $(IMG) fat-image sfs-image
	@echo "[input] keyboard gate: boot + QEMU sendkey -> IRQ1 -> SYS_INPUT_POLL..."
	@rm -f build/input.log /tmp/pinput.sock
	@bash tools/qemu_runner/input_inject.sh build/input.log /tmp/pinput.sock &
	@timeout 90 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -monitor unix:/tmp/pinput.sock,server,nowait \
	    -serial file:build/input.log -display none -no-reboot || true
	@grep -q PRADYOS_INPUT_OK build/input.log || { echo "[input] FAIL — key did not reach ring 3"; tail -20 build/input.log; exit 1; }
	@echo "[input] PASS — $$(grep -a PRADYOS_INPUT_OK build/input.log | head -1)"

# Layer-7 compositor gate (DDR-704): the in-house full-screen compositor renders
# the sovereign desktop over the GPU framebuffer (PRADYOS_COMPOSITOR_OK), then the
# harness injects 'm' and 's' via QEMU sendkey (real IRQ1). The compositor flips
# the mode (SYS_SET_MODE) and re-renders; the manual->sovereign round-trip ending
# in PRADYOS_COMPOSITOR_MODE SOVEREIGN proves keyboard -> mode -> framebuffer.
smoke-compositor: $(IMG) fat-image sfs-image
	@echo "[comp] compositor gate: boot(GPU) + sendkey m/s -> mode -> framebuffer..."
	@rm -f build/comp.log /tmp/pcomp.sock
	@bash tools/qemu_runner/input_inject.sh build/comp.log /tmp/pcomp.sock PRADYOS_COMPOSITOR_OK "m s" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/pcomp.sock,server,nowait \
	    -serial file:build/comp.log -display none -no-reboot || true
	@grep -q PRADYOS_COMPOSITOR_OK build/comp.log || { echo "[comp] FAIL — compositor did not render"; tail -20 build/comp.log; exit 1; }
	@grep -q "PRADYOS_COMPOSITOR_MODE SOVEREIGN" build/comp.log || { echo "[comp] FAIL — key-driven mode flip not confirmed"; tail -20 build/comp.log; exit 1; }
	@# DDR-893 (item 39): MANUAL is a DIFFERENT desktop, not a relabelled one.
	@# These assert STRUCTURE — a taskbar, a menu bar, and the absence of the
	@# agent panel — because a title string is something either layout could
	@# print, which is exactly the mode-flag design the item rules out.
	@grep -q 'PRADYOS_MANUAL_TASKBAR_OK' build/comp.log || { echo "[comp] FAIL: MANUAL has no taskbar (DDR-893)"; tail -20 build/comp.log; exit 1; }
	@grep -q 'PRADYOS_MANUAL_MENUBAR_OK' build/comp.log || { echo "[comp] FAIL: MANUAL has no menu bar (DDR-893)"; tail -20 build/comp.log; exit 1; }
	@grep -q 'PRADYOS_MANUAL_NO_AGENT_PANEL' build/comp.log || { echo "[comp] FAIL: MANUAL still renders the Sovereign agent panel (DDR-893)"; tail -20 build/comp.log; exit 1; }
	@echo "[comp] PASS — desktop rendered + keyboard-driven mode flip confirmed."

# DDR-746 ACPI poweroff gate: boot(GPU) so the sovereign compositor runs, wait
# for it, then sendkey 'p' -> SYS_POWEROFF -> ACPI S5. QEMU has no -no-shutdown,
# so S5 makes it exit; the gate asserts the kernel's pre-write PRADYOS_POWEROFF
# sentinel (and the compositor's key-path marker), and no panic. Only this gate
# sends 'p', so every other boot is unaffected.
smoke-poweroff: $(IMG) fat-image sfs-image
	@echo "[power] poweroff gate: boot(GPU) + sendkey p -> SYS_POWEROFF -> ACPI S5..."
	@rm -f build/power.log /tmp/ppower.sock
	@bash tools/qemu_runner/input_inject.sh build/power.log /tmp/ppower.sock PRADYOS_COMPOSITOR_OK "p" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/ppower.sock,server,nowait \
	    -serial file:build/power.log -display none -no-reboot || true
	@grep -q "PRADYOS_COMPOSITOR_POWEROFF" build/power.log || { echo "[power] FAIL — compositor did not issue SYS_POWEROFF"; tail -20 build/power.log; exit 1; }
	@grep -q "PRADYOS_POWEROFF" build/power.log || { echo "[power] FAIL — kernel ACPI S5 path not reached"; tail -20 build/power.log; exit 1; }
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/power.log; then echo "[power] FAIL: kernel panic"; tail -20 build/power.log; exit 1; fi
	@echo "[power] PASS — sovereign SYS_POWEROFF reached the ACPI S5 poweroff path."

# DDR-747 ACPI reboot gate: like smoke-poweroff but sendkey 'b' -> SYS_REBOOT ->
# ACPI/PC reset. QEMU runs with -no-reboot, so a CPU reset makes it exit; the gate
# asserts the kernel's pre-reset PRADYOS_REBOOT sentinel + the compositor marker,
# and no panic. Only this gate sends 'b', so every other boot is unaffected.
smoke-reboot: $(IMG) fat-image sfs-image
	@echo "[reboot] reboot gate: boot(GPU) + sendkey b -> SYS_REBOOT -> ACPI/PC reset..."
	@rm -f build/reboot.log /tmp/preboot.sock
	@bash tools/qemu_runner/input_inject.sh build/reboot.log /tmp/preboot.sock PRADYOS_COMPOSITOR_OK "b" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/preboot.sock,server,nowait \
	    -serial file:build/reboot.log -display none -no-reboot || true
	@grep -q "PRADYOS_COMPOSITOR_REBOOT" build/reboot.log || { echo "[reboot] FAIL — compositor did not issue SYS_REBOOT"; tail -20 build/reboot.log; exit 1; }
	@grep -q "PRADYOS_REBOOT" build/reboot.log || { echo "[reboot] FAIL — kernel reset path not reached"; tail -20 build/reboot.log; exit 1; }
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/reboot.log; then echo "[reboot] FAIL: kernel panic"; tail -20 build/reboot.log; exit 1; fi
	@echo "[reboot] PASS — sovereign SYS_REBOOT reached the ACPI/PC reset path."

# Layer-7 pointer gate (DDR-705): boot with the GPU + a virtio-tablet; the
# compositor renders the desktop, then the harness injects an absolute move + a
# left click via QMP input-send-event (real virtio-input path). The pointer state
# reaches ring 3 and the compositor draws a cursor + prints PRADYOS_MOUSE_OK.
smoke-mouse: $(IMG) fat-image sfs-image
	@echo "[mouse] pointer gate: boot(GPU+tablet) + QMP move/click -> SYS_MOUSE_POLL..."
	@rm -f build/mouse.log /tmp/pmouse.sock
	@bash tools/qemu_runner/mouse_inject.sh build/mouse.log /tmp/pmouse.sock PRADYOS_AMBIANCE_OK &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pmouse.sock,server,nowait \
	    -serial file:build/mouse.log -display none -no-reboot || true
	@grep -q "\[input\] virtio pointer up" build/mouse.log || { echo "[mouse] FAIL — pointer driver did not come up"; tail -20 build/mouse.log; exit 1; }
	@grep -q PRADYOS_MOUSE_OK build/mouse.log || { echo "[mouse] FAIL — click did not reach ring 3"; tail -20 build/mouse.log; exit 1; }
	@grep -q PRADYOS_RIPPLE_OK build/mouse.log || { echo "[mouse] FAIL — no click ripple (DDR-727)"; tail -20 build/mouse.log; exit 1; }
	@echo "[mouse] PASS — $$(grep -a PRADYOS_MOUSE_OK build/mouse.log | head -1)"

# Layer-7 agent-card click gate (DDR-713): boot GPU+tablet; the daemon lights
# KRYOS (slot 0); the harness clicks agent card 1 (PRAX) via QMP input-send-event;
# the sovereign compositor triggers that agent (SYS_SPAWN_AGENT) and prints
# PRADYOS_AGENT_TRIGGER. Proves desktop pointer -> AETHER. DDR-730: 'active' is
# now LIVE (a slot lights only while its agent's tcb is alive), so the old
# "AGENT PRAX active" serial assertion became a race against a millisecond-lived
# test agent; the deterministic witness is TRIGGER followed by AGENT_DONE (the
# clicked agent genuinely ran to completion).
smoke-agent-click: $(IMG) fat-image sfs-image
	@echo "[aclick] agent-card click gate: boot(GPU+tablet) + QMP click card 1 -> SYS_SPAWN_AGENT..."
	@rm -f build/aclick.log /tmp/paclick.sock
	@ABSX=29250 ABSY=5632 bash tools/qemu_runner/mouse_inject.sh build/aclick.log /tmp/paclick.sock PRADYOS_AGENTS_OK &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/paclick.sock,server,nowait \
	    -serial file:build/aclick.log -display none -no-reboot || true
	@grep -q "PRADYOS_AGENT_TRIGGER name=PRAX slot=1" build/aclick.log || { echo "[aclick] FAIL — card click did not trigger the agent"; tail -20 build/aclick.log; exit 1; }
	@# DDR-896: dump the AGENT-relevant lines plus a wide tail on failure. The old
	@# `tail -20` could not show whether PRADYOS_AGENT_START ever printed, which
	@# made every hypothesis about this gate untestable from a CI log.
	@awk '/PRADYOS_AGENT_TRIGGER name=PRAX/{t=1} t&&/PRADYOS_AGENT_DONE/{ok=1} END{exit !ok}' build/aclick.log || { echo "[aclick] FAIL — the clicked PRAX agent did not run to completion"; echo "--- agent/rate lines (DDR-896) ---"; grep -aE 'PRADYOS_AGENT_|AGENT_RATE_LIMITED|AETHER_AGENT_|sys_exit' build/aclick.log || echo "(none)"; echo "--- tail 200 ---"; tail -200 build/aclick.log; exit 1; }
	@echo "[aclick] PASS — $$(grep -a PRADYOS_AGENT_TRIGGER build/aclick.log | head -1)"

# Layer-7 per-client surface gate (DDR-706): a ring-3 client creates+commits a
# surface; the compositor composites it onto the desktop. Needs the GPU.
smoke-surface: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_SURFACE_CLIENT_OK\nPRADYOS_SURFACE_OK 0')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 window-drag gate (DDR-710): the compositor draws title-bar decorations
# and supports drag-to-move. The harness drags window B by its title bar via QMP;
# the compositor raises+moves it (PRADYOS_DRAG_START / PRADYOS_DRAG). Needs GPU+tablet.
smoke-drag: $(IMG) fat-image sfs-image
	@echo "[drag] window-drag gate: boot(GPU+tablet) + QMP title-bar drag -> SYS_SURFACE_MOVE..."
	@rm -f build/drag.log /tmp/pdrag.sock
	@DG_ID=1 bash tools/qemu_runner/drag_inject.sh build/drag.log /tmp/pdrag.sock PRADYOS_AMBIANCE_OK &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pdrag.sock,server,nowait \
	    -serial file:build/drag.log -display none -no-reboot || true
	@grep -q PRADYOS_DRAG_START build/drag.log || { echo "[drag] FAIL — drag did not start on the title bar"; tail -20 build/drag.log; exit 1; }
	@grep -q "PRADYOS_DRAG id=" build/drag.log || { echo "[drag] FAIL — window did not move"; tail -20 build/drag.log; exit 1; }
	@echo "[drag] PASS — $$(grep -a 'PRADYOS_DRAG id=' build/drag.log | head -1)"

# Layer-7 window close+resize gate (DDR-711): the client (surfacetest) creates a
# third window C, resizes it to 96x96 (PRADYOS_RESIZE_OK), then closes it
# (PRADYOS_CLOSE_OK); the compositor recomposites when the live set shrinks and
# prints PRADYOS_SURFACE_GONE. Client-driven (no QMP); needs the GPU.
smoke-winops: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_RESIZE_OK\nPRADYOS_CLOSE_OK\nPRADYOS_SURFACE_GONE')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 startup-ordering gate (DDR-911). The live set MUST be observed at 3
# before it shrinks to 2. surfacetest used to close its third window on a loop-
# iteration count, so when item 16's fair-share pick changed its CPU share the
# window was created and destroyed before the compositor's first poll — the set
# never reached 3, and two gates failed for a reason neither could express.
#
# Asserting ZORDER with three ids is what stops that recurring silently: it fails
# whenever the third window is unobservable, whatever the cause.
smoke-wmorder: $(IMG) fat-image sfs-image
	@echo "[wmorder] startup ordering: compositor must observe the 3-set before it shrinks..."
	@rm -f build/wmorder.log
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -serial file:build/wmorder.log -display none -no-reboot || true
	@grep -aq "PRADYOS_ZORDER 0 1 2" build/wmorder.log || { \
	    echo "[wmorder] FAIL — the compositor never observed three surfaces."; \
	    echo "          ZORDER lines seen:"; grep -a PRADYOS_ZORDER build/wmorder.log | head -5; \
	    exit 1; }
	@grep -aq PRADYOS_SURFACE_GONE build/wmorder.log || { \
	    echo "[wmorder] FAIL — set reached 3 but never shrank; close path broken"; exit 1; }
	@echo "[wmorder] PASS — $$(grep -a 'PRADYOS_ZORDER 0 1 2' build/wmorder.log | head -1) then shrink"

# Layer-7 surface-destroy gate (DDR-729, -smp 4 so the exit-reap runs cross-CPU):
# surfdestroytest proves the 16-slot table reclaims on churn/close, reuses freed
# slots, and — the lifecycle hole this slice closes — reclaims a child's surfaces
# when it EXITS without SYS_SURFACE_CLOSE. GPU-independent (surfaces are PMM).
smoke-surfdestroy: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SURFDESTROY_CHURN_OK\nPRADYOS_SURFDESTROY_REUSE_OK\nPRADYOS_SURFDESTROY_EXIT_OK\nPRADYOS_SURFDESTROY_OK')" \
	FORBIDDEN_SENTINEL="SURFDESTROY FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# APIC stage-A gate (DDR-714): the MADT-discovered LAPIC comes up and the APIC
# timer takes over the 100 Hz tick (PIT masked); every later sentinel in the
# boot (scheduler, FS, user) implicitly proves the new tick drives the system.
smoke-apic: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf '[apic] up id=\n[apic] timer 100Hz (PIT masked)')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SMP stage-B gate (ADR-029): boot 4 vCPUs; the BSP INIT-SIPIs the 3 APs through
# the 0x8000 trampoline into long mode; each announces and parks. Proves the
# MP-init protocol end-to-end (MADT ids -> IPIs -> real->long mode -> C).
smoke-smp: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 tss OK\n[smp] cpu 2 tss OK\n[smp] cpu 3 tss OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="tss FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# APs-in-scheduler gate (ADR-031 cap-2b): the BSP spawns several READY kernel
# probe threads and kicks the APs; a probe reports a non-BSP cpu_idx, proving a
# ready-ring thread executed on an AP (not just a directed mailbox job).
smoke-smpsched: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] sched cross-CPU OK')" \
	FORBIDDEN_SENTINEL="cross-CPU FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AP preemption gate (ADR-031 cap-3): each AP arms its own LAPIC timer, so a
# non-BSP CPU's per-CPU tick counter advances — proving timer-driven preemption
# on APs (under cap-2b it would stay 0).
smoke-smppreempt: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] ap preempt OK')" \
	FORBIDDEN_SENTINEL="ap preempt FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# User-on-AP gate (ADR-031 cap-4, the capstone): a ring-3 thread is claimed and
# run by a non-BSP CPU, with the user programs still passing their own sentinels
# (they must run CORRECTLY on APs, not merely run).
smoke-smpuser: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] user on AP OK\nHELLO FROM RING-3\nPRADYOS_MUSL_OK')" \
	FORBIDDEN_SENTINEL="user on AP FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Runqueue stress gate (DDR-SMP-rq-1): 24 kernel threads in 3 waves over the
# per-CPU ready queues (steal + wake spread them); all must complete.
smoke-rqstress: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] rqstress OK\n[sched] steal local=')" \
	FORBIDDEN_SENTINEL="rqstress FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Multi-in-flight gate (DDR-BLK-1): two threads keep requests outstanding on
# one disk concurrently (per-request slots; the one-in-flight mutex is gone),
# each round-tripping its own reads cleanly.
smoke-blkmq: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[blk] multi-inflight OK\n[sfs] lz4+tags compress/readback/tag OK')" \
	FORBIDDEN_SENTINEL="multi-inflight FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-790 (P0, operator decision 2026-07-28): run smoke-blkmq against a kernel
# built with PIPE_TRACE=1 so the create/destroy traces are actually reachable.
#
# It has to be its own target because PIPE_TRACE is COMPILE-TIME and CI builds a
# single shared image: flipping the flag globally would put high-volume traces
# into every later gate, and DDR-790 already proved that evicts smoke-dmesg's
# log-ring marker. So this rebuilds with traces, runs only this gate, then
# rebuilds clean — the tree and every other gate are left exactly as before.
#
# make does not track CFLAGS changes, so the objects are removed explicitly or
# the flag silently does nothing (a stale-image trap this project has hit twice).
.PHONY: smoke-blkmq-trace
smoke-blkmq-trace: fat-image sfs-image
	@echo "[blkmq-trace] rebuilding kernel with PIPE_TRACE=1 (DDR-790)"
	rm -f $(BUILD_DIR)/pipe.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG)
	$(MAKE) image PIPE_TRACE=1
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[blk] multi-inflight OK\n[sfs] lz4+tags compress/readback/tag OK')" \
	FORBIDDEN_SENTINEL="multi-inflight FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG) ; \
	  rc=$$? ; \
	  echo "[blkmq-trace] restoring untraced kernel" ; \
	  rm -f $(BUILD_DIR)/pipe.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG) ; \
	  $(MAKE) image ; \
	  exit $$rc

# F#68/DDR-795: the sealed objective-function root page. The probe reads it
# (METRIC_READ_OK) and then stores to it; the store must fault and be converted
# into a clean process kill, so METRIC_WX_FAIL can never print.
#
# Three assertions, because two were not enough:
#   METRIC_READ_OK              the page is mapped and the header is stamped
#   cr2=0x00007FFFFFEFF040      the fault happened at METRIC_USER_VA+0x40 —
#                               exactly the offset the probe stores to
#   no METRIC_WX_FAIL           the store did not reach memory
#
# The cr2 assertion is the one that makes this discriminating. Absence of
# METRIC_WX_FAIL alone is equally consistent with the probe dying earlier for an
# unrelated reason; requiring the fault AT THE RIGHT ADDRESS proves the mapping
# refused the write rather than something else killing the process first.
# DDR-800 (R1): the sovereign egress exemption must still WORK and must now be
# RECORDED. The probe runs sovereign with is_net=0 to a host that is not on the
# allowlist, so the operator flag is the only reason it gets through.
#
# An implementation that keeps the bypass but forgets the audit record passes
# the connect and fails this gate — which is the whole point of the slice.
smoke-sovereign-egress: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='PRADYOS_SOVEGRESS_AUDITED' \
	FORBIDDEN_SENTINEL='SOVEGRESS FAIL' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-801 (R3): every egress decision must name its destination. The probe runs
# with CAP_NET but NOT sovereign, and checks the allowed case AND the denied
# case for the SAME host on a different port — so an implementation that records
# the host but drops the port emits two identical action_ids and cannot satisfy
# both assertions.
smoke-egress-audit: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL='PRADYOS_EGRESS_AUDITED' \
	FORBIDDEN_SENTINEL='EGRESSAUDIT FAIL' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-797: the serial console must not be flooded. Asserts on VOLUME, which
# boot_test.sh cannot — a boot can contain every required sentinel and still be
# 83% binary garbage, which is exactly the state this gate now prevents.
smoke-serialflood: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 MAX_BYTES=32768 bash tools/qemu_runner/flood_gate.sh $(IMG)

# DDR-796 (BUG-1): SYS_CLOCK must never run backwards under -smp 4. CMOS access
# is a two-port sequence over chipset-global state; unserialised, two CPUs
# interleave and each reads the other's register.
#
# This tests the INVARIANT, not the symptom. BUG-1 presented three inferential
# steps away — as a metrics probe reporting "agent never observed as scheduled"
# because its 120-second window had collapsed to zero — which is why it went
# unattributed across four CI runs and four local attempts.
smoke-rtc-smp: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL='PRADYOS_RTC_MONO_OK' \
	FORBIDDEN_SENTINEL='RTC_MONO FAIL' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-metric: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 \
	EXTRA_SENTINEL="$$(printf 'METRIC_READ_OK\ncr2=0x00007FFFFFEFF040')" \
	FORBIDDEN_SENTINEL='METRIC_WX_FAIL' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-777/DDR-791 (BUG-1): run the gate that has failed most often, against a
# kernel built with BSP_LIVENESS=1, so the churn block reports where the BSP got
# to before it stopped progressing.
#
# The three-way read (DDR-777):
#   no [bsp] lines at all         -> the BSP never reached the churn block; the
#                                    stall is EARLIER than DDR-775 assumed.
#   [bsp] lines stop at iter=N,
#   and [hb] keeps ticking        -> the BSP is wedged INSIDE churn iteration N,
#                                    with the timer alive: an SFS/allocator stall.
#   [bsp] runs to iter=39 and
#   "btree churn OK" prints       -> churn completed; the stall is AFTER it, and
#                                    the SFS allocator is exonerated.
#
# Own target for the same reason as smoke-blkmq-trace: the flag is COMPILE-TIME
# and CI builds one shared image, so enabling it globally would put per-iteration
# output into every later gate — which DDR-790 already proved evicts smoke-dmesg's
# marker from the last-4 KiB log ring.
smoke-rqstress-liveness: fat-image sfs-image
	@echo "[bsp-liveness] rebuilding kernel with BSP_LIVENESS=1 (DDR-777)"
	rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG)
	$(MAKE) image BSP_LIVENESS=1
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] rqstress OK')" \
	FORBIDDEN_SENTINEL="rqstress FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG) ; \
	  rc=$$? ; \
	  echo "[bsp-liveness] restoring untraced kernel" ; \
	  rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG) ; \
	  $(MAKE) image ; \
	  exit $$rc

# DDR-777/790/791 (BUG-1): smoke-fs against a BSP_LIVENESS=1 kernel.
#
# smoke-fs is the gate that actually caught the wedge (CI run 32259190462):
# neither '[sfs] btree churn OK' nor '[sfs] churn FAIL' printed, so the BSP
# stopped INSIDE the 40-iteration DDR-763 churn loop -- it neither finished nor
# reported a failure. The downstream [pdrive] and freelist sentinels were simply
# never reached, which is why the gate blamed the freelist.
#
# smoke-rqstress-liveness already does this for the rqstress gate; this is the
# same harness pointed at the gate that reproduces. Same three-way read (DDR-777):
#   no [bsp] lines            -> BSP never reached churn; stall is EARLIER.
#   [bsp] stops at iter=N,
#     [hb] still ticking      -> wedged INSIDE iteration N, timer alive.
#   [bsp] reaches iter=39 and
#     'btree churn OK' prints -> churn completed; stall is AFTER it.
#
# Own target, NOT added to the shard matrix: BSP_LIVENESS is COMPILE-TIME and CI
# builds one shared image, so enabling it globally would put per-iteration output
# into every later gate -- DDR-790 proved that evicts smoke-dmesg's marker from
# the last-4 KiB log ring. Diagnostic only; remove once BUG-1 is closed.
smoke-fs-liveness: fat-image sfs-image
	@echo "[bsp-liveness] rebuilding kernel with BSP_LIVENESS=1 (DDR-777)"
	rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG)
	$(MAKE) image BSP_LIVENESS=1
	SERIAL_LOG=build/gatelogs/fsliveness_serial.log \
	    TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'msix vec=56\nPRADYOS filesystem works!\nnested file ok\nlong name read works\n[rtc] 20\nkernel wrote this\ncreated+deleted /TMP.TXT OK\ncreate/lookup OK\nbyte-exact OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK\n[sfs] freelist persist OK\n[pdrive] workspace OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG) ; \
	  rc=$$? ; \
	  echo "[bsp-liveness] serial kept at build/gatelogs/fsliveness_serial.log" ; \
	  echo "[bsp-liveness] churn trace (last 3):" ; \
	  if [ ! -s build/gatelogs/fsliveness_serial.log ]; then echo '[bsp-liveness] SERIAL CAPTURE MISSING OR EMPTY - harness broken, not a verdict'; elif grep -aq 'churn iter=' build/gatelogs/fsliveness_serial.log; then grep -a 'churn iter=' build/gatelogs/fsliveness_serial.log | tail -3; else echo '[bsp-liveness] NO [bsp] LINES - BSP_LIVENESS not compiled in'; fi ; \
	  grep -ac 'btree churn OK' build/gatelogs/fsliveness_serial.log | sed 's/^/[bsp-liveness] btree-churn-OK count: /' ; \
	  echo "[bsp-liveness] restoring untraced kernel" ; \
	  rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG) ; \
	  $(MAKE) image ; \
	  exit $$rc

# ---------------------------------------------------------------------------
# ADR-034 — aarch64 / riscv64 bootstrap. ADDITIVE: these targets share no
# object, flag or rule with the x86_64 build, so a break here cannot be a break
# there. Scope is BOOT ONLY — the smoke-gate set is not ported yet.
# ---------------------------------------------------------------------------
ARM64_DIR   := kernel/arch/aarch64
RV64_DIR    := kernel/arch/riscv64
ARM64_ELF   := build/kernel-aarch64.elf
RV64_ELF    := build/kernel-riscv64.elf

kernel-aarch64: $(ARM64_ELF)
$(ARM64_ELF): $(ARM64_DIR)/boot.S $(ARM64_DIR)/start.c $(ARM64_DIR)/kernel.ld
	@mkdir -p build
	$(CC) $(CFLAGS_ARM64) -c $(ARM64_DIR)/boot.S  -o build/aarch64_boot.o
	$(CC) $(CFLAGS_ARM64) -c $(ARM64_DIR)/start.c -o build/aarch64_start.o
	$(LD) -nostdlib -T $(ARM64_DIR)/kernel.ld -o $@ \
	      build/aarch64_boot.o build/aarch64_start.o
	@echo "kernel-aarch64: $@"

kernel-riscv64: $(RV64_ELF)
$(RV64_ELF): $(RV64_DIR)/boot.S $(RV64_DIR)/start.c $(RV64_DIR)/kernel.ld
	@mkdir -p build
	$(CC) $(CFLAGS_RV64) -c $(RV64_DIR)/boot.S  -o build/riscv64_boot.o
	$(CC) $(CFLAGS_RV64) -c $(RV64_DIR)/start.c -o build/riscv64_start.o
	$(LD) -nostdlib -T $(RV64_DIR)/kernel.ld -o $@ \
	      build/riscv64_boot.o build/riscv64_start.o
	@echo "kernel-riscv64: $@"

# Boot gates. Own runner rather than boot_test.sh: that script hard-codes
# qemu-system-x86_64 and the whole virtio/FAT/SFS disk set, none of which this
# boot-only slice has. Reusing it would mean threading arch through every disk
# option for a gate that boots a bare ELF.
smoke-aarch64: kernel-aarch64
	ARCH=aarch64 QEMU=qemu-system-aarch64 KERNEL_ELF=$(ARM64_ELF) \
	    bash tools/qemu_runner/boot_arch.sh

smoke-riscv64: kernel-riscv64
	ARCH=riscv64 QEMU=qemu-system-riscv64 KERNEL_ELF=$(RV64_ELF) \
	    bash tools/qemu_runner/boot_arch.sh

# DDR-759 SMP audit gate (M1 3/3): 4 concurrent workers under -smp 4 each read a
# sector 64x and verify the bytes against a single-threaded reference checksum, so
# a completion mis-routed to the wrong DDR-BLK-1 slot (wrong data) is caught (not
# just read-success, which blkmq_proof already covers).
smoke-blk-integrity: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] blk integrity OK')" \
	FORBIDDEN_SENTINEL="blk integrity FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-760 persistent-SFS-root gate (M2): after the destructive SFS self-tests, the
# kernel reformats blk2 clean, provisions /etc/aether/config, and roots a probe
# there. The probe reads the config through its SFS root -> PRADYOS_SFSROOT_OK,
# proving a process can durably root at a clean, provisioned SFS volume.
smoke-sfsroot: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SFSROOT_OK')" \
	FORBIDDEN_SENTINEL="SFSROOT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-764 large-write gate: a ring-3 probe writes 8 KiB to the SFS root in one
# SYS_WRITE and reads it back. With the old 256-byte FD_VFS chunk this short-wrote
# at ~1 KiB (5th SFS extent rejected); the 4 KiB chunk lands 8 KiB in 2 extents.
smoke-vfs-bigwrite: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_BIGWRITE_OK')" \
	FORBIDDEN_SENTINEL="BIGWRITE FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-802 privacy netfilter gate, selected per-boot by DDR-804 so the probe's
# global privacy toggle cannot perturb the concurrent egress probes in every
# other gate. The probe is SOVEREIGN + CAP_NET on an ALLOWLISTED destination:
# a connect that would otherwise unambiguously succeed, so an implementation
# that does nothing fails here. It also proves ORDERING — a sovereign connect
# to an off-allowlist port must NOT leave an AR_SOVEREIGN_BYPASS record, which
# it would if the DDR-800 bypass ran ahead of the privacy check.
smoke-privacy-netfilter: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=privnet \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_PRIVACY_DENIED_OK\nPRADYOS_PRIVACY_AUDIT_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRIVACYNET FAIL\nPRADYOS_SOVEREIGN_BYPASSED')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-805 SIGPIPE gate, selected per-boot by DDR-804 so the probe exists only
# here. Discriminating on SURVIVAL, not on exit status: the kernel sets
# exit_status = -1 for every default-terminate signal and records no signal
# number, so asserting "$$? == 13" would need a fourth edit changing SIGKILL
# and SIGTERM too. The CONTROL marker proves the kill is specific to the
# readerless case — a kernel that killed every pipe writer would otherwise pass.
smoke-sigpipe: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=sigpipe \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SIGPIPE_CONTROL_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_SIGPIPE_STUB\nSIGPIPE FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-811 SHA-256 vector gate. Opt-in per DDR-804 so the probe exists only
# here. The 1M-'a' vector is load-bearing: vectors 1-3 fit in two blocks and
# would pass against a wrong length counter or a broken partial-block carry.
# DDR-814 AGS goal signing. Three arms: round-trip, tampered goal, wrong key.
# The last two are the gate's whole point — a verify() that always returns 0
# passes arm 1, and one that ignores the public key passes arms 1-2.
# DDR-815 ACC rotation. ONE ELF spawned twice at different privilege; it tells
# the instances apart because rotate-unknown-channel returns -ENOENT to a
# sovereign caller and -EPERM to an agent. Both sentinels are required, so a run
# where only one instance spawns is a FAILURE, not a silent half-pass.
# DDR-834 credential vault. Arm 3 tampers /VAULT.BIN through the ordinary file
# syscalls and requires the read to be REJECTED - arms 1-2 pass on a vault that
# stores plaintext, so that arm is the one carrying the feature. QEMU_RNG is
# required: vault_put draws a fresh nonce and refuses to store without one.
# DDR-836 agent memory. Arm 3 (overwrite replaces, no stale tail bytes) and arm 4
# (denied without CAP_MEMORY) carry the weight - arms 1-2 pass on an append-only
# store with no capability check at all.
# DDR-837 checkpoint/resume. Asserts on the target's OBSERVED ps state, never on
# elapsed time - a timing assertion on a scheduling feature is a flake generator.
# Arms 1-2 (freeze reaches THREAD_BLOCKED, resume leaves it) are the feature;
# arms 3-4 (-ESRCH on unknown pid, -EPERM without sovereignty) are the guards.
# DDR-838 spawn-depth cap. Asserts BOTH halves: depths 1-3 must succeed and the
# fourth generation must be refused with -EAGAIN at depth 3 exactly. A cap that
# refuses everything also bounds replication and would pass a refusal-only gate.
# DDR-839 DAG action queue. Arms 2 and 3 are a PAIR: a queue that refuses every
# child approval passes arm 2 and is useless; one that ignores parents passes
# arm 3 and enforces nothing. Both sentinels required, so a run where only the
# agent half completes is a FAILURE rather than a silent half-pass.
# DDR-841 Group 1: reproducible build env, VirtualBox runner, chipset matrix.
#
# smoke-chipset boots the SAME image across the x86_64 variants QEMU can give
# us. Every other gate in this tree boots -M q35 only, so a q35-specific
# assumption would never be caught. The AMD arm is the point: every CPU-feature
# assumption here was written against an Intel-flavoured qemu64, and a missing
# CPUID guard shows up there and nowhere else.
CHIPSET_VARIANTS := q35/qemu64 pc/qemu64 q35/Nehalem q35/Opteron_G5

smoke-chipset: $(IMG) fat-image sfs-image
	@fail=0; \
	for v in $(CHIPSET_VARIANTS); do \
	  m=$${v%%/*}; c=$${v##*/}; \
	  echo "[chipset] === -M $$m -cpu $$c ==="; \
	  if TIMEOUT_S=90 QEMU_MACHINE=$$m QEMU_CPU=$$c \
	     bash tools/qemu_runner/boot_test.sh $(IMG) >/tmp/cs_$$m_$$c.log 2>&1; then \
	    echo "[chipset] PASS -M $$m -cpu $$c"; \
	  else \
	    echo "[chipset] FAIL -M $$m -cpu $$c"; tail -15 /tmp/cs_$$m_$$c.log; fail=1; \
	  fi; \
	done; \
	test $$fail -eq 0 || { echo '[chipset] FAIL - a variant did not boot'; exit 1; }
	@echo '[chipset] PASS - all $(words $(CHIPSET_VARIANTS)) x86_64 variants booted'

# Item 2: build inside the pinned container. Not run in CI (docker-in-docker is
# a separate can of worms); ci-docker-check validates the Dockerfile instead.
.PHONY: docker-image docker-build ci-docker-check ci-vbox-check vbox-boot
docker-image:
	docker build -t pradyos-build .

docker-build: docker-image
	docker run --rm -v "$$PWD":/src -w /src pradyos-build make image

# The Dockerfile must pin a base tag and must not add unpinned third-party
# archives — an apt PPA would reintroduce the drift the image exists to remove.
ci-docker-check:
	@test -f Dockerfile || { echo 'ci-docker-check: FAIL — no Dockerfile'; exit 1; }
	@grep -qE '^FROM ubuntu:24[.]04' Dockerfile || { echo 'ci-docker-check: FAIL — base image not pinned to ubuntu:24.04'; exit 1; }
	@! grep -qiE 'add-apt-repository|ppa:' Dockerfile || { echo 'ci-docker-check: FAIL — unpinned third-party archive in Dockerfile'; exit 1; }
	@echo 'ci-docker-check: OK — Dockerfile pins ubuntu:24.04, no unpinned archives'

# Item 4: the runner cannot execute in CI (no VirtualBox, nesting unsupported).
# CI validates what it honestly can — that the script parses — and the operator
# runs the real boot for D.2. The script exits 77 when VBoxManage is absent,
# never 0, so a green pipeline can never imply a boot that did not happen.
ci-vbox-check:
	@test -x tools/vbox_runner/run_vbox.sh || { echo 'ci-vbox-check: FAIL — runner missing or not executable'; exit 1; }
	@bash -n tools/vbox_runner/run_vbox.sh || { echo 'ci-vbox-check: FAIL — syntax error'; exit 1; }
	@grep -q 'exit 77' tools/vbox_runner/run_vbox.sh || { echo 'ci-vbox-check: FAIL — runner must exit 77 when VirtualBox is absent, not 0'; exit 1; }
	@echo 'ci-vbox-check: OK — runner parses and fails loudly when VirtualBox is absent'

vbox-boot: $(IMG)
	bash tools/vbox_runner/run_vbox.sh $(IMG)

# DDR-842 item 6. FOUR sentinels required. The sov-only arm is the point: if
# CAP_SOVEREIGN alone sufficed, CAP_REWRITE would be decoration, and a gate that
# only checked 'unprivileged is denied' would pass against exactly that bug.
# DDR-844: S1-S8 attack gate. Every OTHER gate asserts a feature works; this one
# asserts attacks FAIL. A refactor that deletes a capability check breaks no
# feature gate - only the refusals stop happening, and nothing else notices.
smoke-invariants: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=invariants \
	EXTRA_SENTINEL="PRADYOS_INVARIANTS_OK S1,S2,S4,S5,S6,S8" \
	FORBIDDEN_SENTINEL="INVARIANT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-844: S5's 'no user-space erase path' cannot be attacked from ring 3 - the
# ABSENCE of a syscall is not something a syscall can test. Asserted at build
# time instead: no erase/clear/reset/purge entry point may be registered against
# the audit log. A runtime arm would have to invent the hole it tests for.
.PHONY: ci-audit-noerase-check
ci-audit-noerase-check:
	@! grep -rniE 'syscall_register\(SYS_[A-Z_]*(ERASE|CLEAR|RESET|PURGE)' kernel/syscall/ \
	  || { echo 'ci-audit-noerase-check: FAIL - an audit erase/clear syscall is registered (S5)'; exit 1; }
	@echo 'ci-audit-noerase-check: OK - no user-space audit erase path exists (S5)'

smoke-coderewrite: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=coderewrite \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_CODEREWRITE_OK\nPRADYOS_CODEREWRITE_SUBMIT_OK\nPRADYOS_CODEREWRITE_SOVONLY_DENIED_OK\nPRADYOS_CODEREWRITE_RWONLY_DENIED_OK')" \
	FORBIDDEN_SENTINEL="CODEREWRITE FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-842 item 7, arm 1: an untouched log verifies clean. The tamper sentinel is
# FORBIDDEN here, so an over-eager verifier that cries tamper on a clean log
# fails this arm instead of looking correct.
smoke-auditchain: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=auditchain \
	EXTRA_SENTINEL="PRADYOS_AUDITCHAIN_INTACT_OK" \
	FORBIDDEN_SENTINEL="$$(printf 'AUDITCHAIN FAIL\nPRADYOS_AUDITCHAIN_TAMPER_DETECTED_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-842 item 7, arm 2 - THE arm that proves the verifier can fail. Ring 3 has
# no write path into the log (that is what S5 asserts), so the corruption is
# injected kernel-side behind a probe flag. Without this, a verify() returning 0
# unconditionally would pass arm 1 and the gate would assert nothing.
smoke-auditchain-tamper: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=auditchain,audittamper \
	EXTRA_SENTINEL="PRADYOS_AUDITCHAIN_TAMPER_DETECTED_OK" \
	FORBIDDEN_SENTINEL="$$(printf 'AUDITCHAIN FAIL\nPRADYOS_AUDITCHAIN_INTACT_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-actiondag: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=actiondag \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ACTIONDAG_OK\nPRADYOS_ACTIONDAG_SUBMIT_OK')" \
	FORBIDDEN_SENTINEL="ACTIONDAG FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-spawndepth: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=spawndepth \
	EXTRA_SENTINEL="PRADYOS_SPAWNDEPTH_OK depth=3" \
	FORBIDDEN_SENTINEL="SPAWNDEPTH FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-checkpoint: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=ckpt \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_CKPT_OK\nPRADYOS_CKPT_DENY_OK')" \
	FORBIDDEN_SENTINEL="CKPT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-agentmem: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=agentmem \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_AGENTMEM_OK\nPRADYOS_AGENTMEM_DENY_OK')" \
	FORBIDDEN_SENTINEL="AGENTMEM FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-vault: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=vault QEMU_RNG=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_VAULT_OK\nPRADYOS_VAULT_DENY_OK')" \
	FORBIDDEN_SENTINEL="VAULT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-acc-rotate: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=accrot QEMU_RNG=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ACCROT_OK\nPRADYOS_ACCROT_DENY_OK')" \
	FORBIDDEN_SENTINEL="ACCROT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-ags: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_PROBES=ags 	EXTRA_SENTINEL="$$(printf 'PRADYOS_AGS_VECTORS_OK')" 	FORBIDDEN_SENTINEL="AGS FAIL" 	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-sha256: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=sha256 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SHA256_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_SHA256_STUB\nSHA256 FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-818 HKDF-SHA256 vectors (RFC 5869 Appendix A). Three cases, each
# covering something the others cannot: TC2's 82-byte OKM is the only one that
# forces the expand loop past T(1), and TC3 is the only one exercising the
# NULL-salt branch (HashLen zero bytes, not an empty string).
# DDR-990: the two-CPU connect/close hammer — the POSITIVE proof of the DDR-987
# lwIP fix that the gate suite cannot supply. smoke-surfdestroy surfaces that
# defect at ~1 run in 20, so a clean 20-run campaign has only ~64% power
# (0.95^20 = 0.358); reaching 95% by sampling needs ~59 boots. This drives the
# named mechanism directly instead: two ring-3 instances on two CPUs issuing
# 20,000 connect/close pairs each, interleaving tcp_seg allocate and free with
# no phase relationship.
#
# QEMU_SMP=4 is load-bearing. On one CPU the two instances serialise and the
# probe measures nothing about a CROSS-CPU race while still printing OK.
#
# conn_err=0 is asserted, not hoped for. If the egress allowlist lacks
# 127.0.0.1:8007 every connect returns an audited -EPERM, the hammer never
# enters lwIP, and it still reaches its sentinel. That is DDR-988 sec.9's
# vacuous gate exactly (smoke-net-fuzz green while 613 of 768 frames were
# dropped), so the absence of errors is checked rather than assumed.
#
# BOTH pids must finish: a run where one instance died and the other completed
# is a single-CPU run wearing a green result. Two OK lines are required.
# DDR-991: PS/2 modifier / extended-key gate. Real keys via QEMU's HMP sendkey
# (genuine IRQ1 path), five arms — see user/modkeystest.c. The two that carry
# the most weight:
#   arm C  Arrow-Up. Arrows are INVISIBLE on the DDR-703 driver: an arrow is
#          `E0 48`, the prefix was swallowed as a break code (0xE0 has bit 7
#          set) and 0x48 was then dropped by `sc >= 0x40`. Arrival IS the proof
#          that 0xE0 is decoded.
#   arm E  an unmodified key AFTER a Ctrl chord. Without break-code handling a
#          modifier latches down forever and a phantom Ctrl turns ordinary
#          typing into control codes — and that regression passes every other
#          arm, which is why this one exists.
smoke-modkeys: $(IMG) fat-image sfs-image
	@echo "[modkeys] input gate: boot + sendkey a/f1/up/ctrl-c/b -> IRQ1 -> NSI 46 + 96..."
	@rm -f build/modkeys.log /tmp/pmodkeys.sock
	@bash tools/qemu_runner/input_inject.sh build/modkeys.log /tmp/pmodkeys.sock \
	    PRADYOS_MODKEYS_WAIT "a f1 up ctrl-c b" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -monitor unix:/tmp/pmodkeys.sock,server,nowait \
	    -fw_cfg name=opt/org.pradyos/probes,string=modkeys \
	    -serial file:build/modkeys.log -display none -no-reboot || true
	@grep -qa "MODKEYS FAIL" build/modkeys.log && { echo "[modkeys] FAIL:"; grep -a "MODKEYS FAIL" build/modkeys.log; exit 1; } || true
	@grep -qa PRADYOS_MODKEYS_OK build/modkeys.log || { echo "[modkeys] FAIL — probe never reported OK"; tail -25 build/modkeys.log; exit 1; }
	@echo "[modkeys] PASS — all five arms"

smoke-nethammer: $(IMG) fat-image sfs-image
	TIMEOUT_S=240 QEMU_SMP=4 QEMU_PROBES=nethammer \
	EXTRA_SENTINEL="$$(printf 'net hammer spawned=2/2\nPRADYOS_NETHAMMER_OK\nconn_err=0')" \
	FORBIDDEN_SENTINEL="$$(printf 'NETHAMMER FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

smoke-hkdf: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=hkdf \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_HKDF_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_HKDF_STUB\nHKDF FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-820 X25519 vectors (RFC 7748). Five checks; the commutativity one
# (a*(b*G) == b*(a*G)) depends on NO published constant and is what makes this
# gate robust against the constants below being recalled rather than fetched.
#
# What this gate does NOT assert: constant-time execution. See DDR-820 — an
# implementation can pass every vector here and still leak the private key
# through timing, and QEMU under TCG cannot observe that.
smoke-x25519: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=x25519 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_X25519_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_X25519_STUB\nX25519 FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)


# DDR-812 metric lockbox. Arms A/B/C only — arm D (ring 3 cannot write the
# page) is already gated by smoke-metric, whose probe stores to
# METRIC_USER_VA and whose gate pins the fault to cr2=...040, i.e. offset 64,
# which is exactly where the lockbox record begins. Duplicating it here would
# add a second probe for one property and a second way for them to disagree.
smoke-lockbox: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=lockbox \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_LOCKBOX_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_LOCKBOX_STUB\nLOCKBOX FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-816 entropy. A gate cannot prove randomness; these arms assert what it
# CAN prove. The likeliest real bug is a driver that appears to work and
# returns the same buffer every time, so the kernel self-test draws twice and
# compares byte-for-byte. Asserting "the call returned 0" would pass against a
# stub that memsets zero.
#
# Every OTHER gate boots with no entropy device, so the suite collectively
# exercises the fail-closed path: if rng_bytes() wrongly returned success with
# zeros, the self-test would print PRADYOS_RNG_STUB and GLOBAL_FORBIDDEN would
# fail all of them.
smoke-rng: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_RNG=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_RNG_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_RNG_STUB\nRNG FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)






# DDR-763 B+tree churn gate: 40x create+write(64K)+unlink of the same path on the
# SFS root drives the inode-entry B+tree well past its first leaf split
# (SFS_LEAF_MAX=14) — coverage no prior gate had. Proves the tree is sound (the
# "B+tree bug" was actually the 1 MiB per-thread write budget; see DDR-763).
smoke-sfs-btree: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf '[sfs] btree churn OK')" \
	FORBIDDEN_SENTINEL="btree churn FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-824 / OPEN-10 reproduction surface. Same probe, -smp 4.
#
# NOT in the shard matrix yet, and the reason is not squeamishness: OPEN-10 has
# hit ~2 of the last 4 CI runs under -smp 4, so registering this now would make
# CI red on a known-open defect and block every unrelated promotion behind it.
# It exists so the defect can be reproduced ON DEMAND with the diagnosis the
# DDR-824 harness change now prints. Register it the moment OPEN-10 is fixed —
# at that point a red here is a regression, which is what a gate is for.
#
# TIMEOUT_S=180, not 90: measured on a WSL2/TCG host, 16 of 20 runs at -smp 4
# exceeded 90 s WITHOUT failing (they never printed OK or FAIL). Three extra
# vCPUs multiply emulation work without adding host parallelism. A 90 s window
# here would measure the host, not the kernel.
smoke-sfs-btree-smp4: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[sfs] btree churn OK')" \
	FORBIDDEN_SENTINEL="btree churn FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-762-v2 SFS free-space GC gate: 300x create+write(64K)+unlink on the SFS root.
# Without the free-extent-run reclaim the ~4800 data blocks exceed the ~4096-block
# volume and a write past the disk fails; with reclaim each unlink's 16-block run
# is reused (exact fit) -> all 300 succeed. Proves snapshot-safe contiguous reuse.
smoke-sfs-gc: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf '[sfs] free-space GC OK')" \
	FORBIDDEN_SENTINEL="free-space GC FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# MSI-X distribution gate (DDR-714C3): the blk vectors target APs; a disk
# completion handler provably ran on a non-BSP CPU while the FS phase's actual
# I/O all still passes (correctness under cross-CPU completion).
smoke-msixap: $(IMG) fat-image sfs-image
	TIMEOUT_S=180 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[blk] msix on AP OK\n[sfs] lz4+tags compress/readback/tag OK')" \
	FORBIDDEN_SENTINEL="msix on AP FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SMP locking gate (ADR-030 stage 1): each AP allocates+frees a PMM page and a
# slab object through the new subsystem spinlocks before parking; all three must
# report locks OK (and none FAIL) alongside full bring-up.
smoke-smplock: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 locks OK\n[smp] cpu 2 locks OK\n[smp] cpu 3 locks OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="locks FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-CPU identity gate (ADR-030 stage 2, DDR-SMP-2): the BSP and every AP
# record + round-trip their percpu entry (LAPIC-ID-indexed this_cpu()).
smoke-percpu: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[percpu] bsp idx=\n[smp] cpu 1 percpu OK\n[smp] cpu 2 percpu OK\n[smp] cpu 3 percpu OK')" \
	FORBIDDEN_SENTINEL="percpu FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SWAPGS gate (ADR-030 stage 3a, DDR-SMP-3a): the %gs:0 percpu self-pointer must
# round-trip from a ring-3-entered syscall (only true when the SWAPGS discipline
# on syscall/isr/usermode transitions is balanced) AND from every parked AP.
smoke-swapgs: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[percpu] gs OK (syscall ctx)\n[smp] cpu 1 percpu OK\n[smp] cpu 2 percpu OK\n[smp] cpu 3 percpu OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="$$(printf 'gs FAIL\npercpu FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AP work-dispatch gate (ADR-030 stage 3c-alpha, DDR-SMP-3c-alpha): the BSP
# posts a job to each idle AP's mailbox + wake IPI (vector 49); every AP runs
# it and reports, then the BSP confirms all mailboxes drained.
smoke-smpjob: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 job OK\n[smp] cpu 2 job OK\n[smp] cpu 3 job OK\n[smp] jobs done=3')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Cross-CPU wake gate (ADR-030 3c-locks-1): a BSP thread blocks; an AP job
# sched_unblocks it (atomic CAS, no ring lock needed for state-only
# transitions); it resumes on the (spinlocked) BSP scheduler.
smoke-crosswake: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cross-wake waiting\n[smp] cross-wake OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-CPU scheduler-state gate (ADR-030 stage 3b, DDR-SMP-3b): current_thread +
# the SYSCALL kstack now live at %gs:8/%gs:16; the probe verifies the running
# thread resolves through percpu from ring-3 syscall context.
smoke-percpu-sched: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[percpu] current OK (syscall ctx)\n[percpu] gs OK (syscall ctx)\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="$$(printf 'current FAIL\ngs FAIL\npercpu FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 visual-richness gate (DDR-712): the compositor renders a per-ambiance
# particle field (NIGHT stars at boot) over the background and frosted-glass agent
# cards, announcing PRADYOS_PARTICLES_OK + PRADYOS_GLASS_OK on its first render.
# Client-driven (no QMP); needs the GPU.
smoke-visual: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_PARTICLES_OK\nPRADYOS_GLASS_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 title+close gate (DDR-715): windows carry title strings (ALPHA/BETA/
# GAMMA) and a close box; the harness clicks window C (GAMMA)'s close box via QMP
# -> the sovereign compositor SYS_SURFACE_CLOSEs it (PRADYOS_WM_CLOSE) and the
# shrink detector repaints (PRADYOS_SURFACE_GONE). Needs GPU + tablet.
smoke-wmclose: $(IMG) fat-image sfs-image
	@echo "[wmclose] title + close-button gate: boot(GPU+tablet) + QMP click GAMMA's close box..."
	@rm -f build/wmclose.log /tmp/pwmclose.sock
	@GEOM_TITLE=GAMMA GEOM_FIELD=close bash tools/qemu_runner/mouse_inject.sh build/wmclose.log /tmp/pwmclose.sock PRADYOS_AMBIANCE_OK "PRADYOS_WM_CLOSE id=" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pwmclose.sock,server,nowait \
	    -serial file:build/wmclose.log -display none -no-reboot || true
	@grep -q PRADYOS_TITLE_OK build/wmclose.log || { echo "[wmclose] FAIL — titles not set"; tail -20 build/wmclose.log; exit 1; }
	@grep -q "PRADYOS_WM_CLOSE id=2" build/wmclose.log || { echo "[wmclose] FAIL — close box click did not close"; tail -20 build/wmclose.log; exit 1; }
	@grep -q PRADYOS_SURFACE_GONE build/wmclose.log || { echo "[wmclose] FAIL — no recomposite after close"; tail -20 build/wmclose.log; exit 1; }
	@echo "[wmclose] PASS — $$(grep -a PRADYOS_WM_CLOSE build/wmclose.log | head -1)"

# Layer-7 minimize gate (DDR-717): the mouse injector clicks B's min box (QMP),
# the window vanishes from the composite (PRADYOS_WM_MIN), then the key injector
# (waiting on PRADYOS_WM_MIN over the HMP monitor) sends 'r' -> restore-all
# (PRADYOS_WM_RESTORE). Needs GPU + tablet + monitor.
smoke-wmmin: $(IMG) fat-image sfs-image
	@echo "[wmmin] minimize gate: boot(GPU+tablet) + QMP click B's min box + sendkey r..."
	@rm -f build/wmmin.log /tmp/pwmmin.sock /tmp/pwmminh.sock
	@GEOM_TITLE=BETA GEOM_FIELD=min bash tools/qemu_runner/mouse_inject.sh build/wmmin.log /tmp/pwmmin.sock PRADYOS_AMBIANCE_OK PRADYOS_WM_MIN &
	@bash tools/qemu_runner/input_inject.sh build/wmmin.log /tmp/pwmminh.sock PRADYOS_WM_MIN "r" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pwmmin.sock,server,nowait \
	    -monitor unix:/tmp/pwmminh.sock,server,nowait \
	    -serial file:build/wmmin.log -display none -no-reboot || true
	@grep -q "PRADYOS_WM_MIN id=1" build/wmmin.log || { echo "[wmmin] FAIL — min box click did not minimize"; tail -20 build/wmmin.log; exit 1; }
	@grep -q PRADYOS_WM_RESTORE build/wmmin.log || { echo "[wmmin] FAIL — r did not restore"; tail -20 build/wmmin.log; exit 1; }
	@echo "[wmmin] PASS — $$(grep -a PRADYOS_WM_MIN build/wmmin.log | head -1) + restore"

# Layer-7 maximize gate (DDR-719): click B's max box -> the compositor saves
# geometry, requests 512x512 via the event channel + moves B to (8,26)
# (PRADYOS_WM_MAX, client PRADYOS_EV_RESIZE_OK w=512 h=512); a second injection
# at the relocated box (keyed on the client's ack) restores (PRADYOS_WM_UNMAX).
smoke-wmmax: $(IMG) fat-image sfs-image
	@echo "[wmmax] maximize gate: boot(GPU+tablet) + QMP max-box click + restore click..."
	@rm -f build/wmmax.log /tmp/pwmmax.sock
	@GEOM_TITLE=BETA GEOM_FIELD=mx bash tools/qemu_runner/mouse_inject.sh build/wmmax.log /tmp/pwmmax.sock PRADYOS_AMBIANCE_OK &
	@GEOM_TITLE=BETA GEOM_FIELD=mx bash tools/qemu_runner/mouse_inject.sh build/wmmax.log /tmp/pwmmax.sock "PRADYOS_EV_RESIZE_OK w=512" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pwmmax.sock,server,nowait \
	    -serial file:build/wmmax.log -display none -no-reboot || true
	@grep -q "PRADYOS_WM_MAX id=1" build/wmmax.log || { echo "[wmmax] FAIL — max box click did not maximize"; tail -20 build/wmmax.log; exit 1; }
	@grep -q "PRADYOS_EV_RESIZE_OK w=512 h=512" build/wmmax.log || { echo "[wmmax] FAIL — client did not honor maximize"; tail -20 build/wmmax.log; exit 1; }
	@grep -q "PRADYOS_WM_UNMAX id=1" build/wmmax.log || { echo "[wmmax] FAIL — restore click did not un-maximize"; tail -20 build/wmmax.log; exit 1; }
	@echo "[wmmax] PASS — $$(grep -a PRADYOS_WM_UNMAX build/wmmax.log | head -1)"

# Layer-7 event-channel resize gate (DDR-718): the harness drags window B's
# bottom-right corner (QMP); the compositor sends SURF_EV_RESIZE_REQ
# (PRADYOS_RESIZE_REQ); the OWNER resizes/redraws/recommits
# (PRADYOS_EV_RESIZE_OK) — authority stays owner-only. Needs GPU + tablet.
smoke-evresize: $(IMG) fat-image sfs-image
	@echo "[evresize] corner drag-resize gate: boot(GPU+tablet) + QMP corner drag..."
	@rm -f build/evresize.log /tmp/pevrs.sock
	@RZ_ID=1 bash tools/qemu_runner/drag_inject.sh build/evresize.log /tmp/pevrs.sock PRADYOS_AMBIANCE_OK &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pevrs.sock,server,nowait \
	    -serial file:build/evresize.log -display none -no-reboot || true
	@# DDR-937: dump the press-dispatch lines on failure. Every press branch
	@# except the resize corner already prints a distinct sentinel, so the log
	@# names which branch swallowed the click — MOUSE_OK means the corner
	@# hit-test missed (with the coordinate it missed with), DRAG_START means it
	@# hit a title bar, WM_MIN/MAX/CLOSE means a window button, and none of them
	@# means the button-down edge was never observed at all. WM_GEOM is included
	@# so the published corner can be compared against the injected point. The
	@# old `tail -20` scrolled all of this past unseen.
	@grep -q "PRADYOS_RESIZE_REQ id=1" build/evresize.log || { echo "[evresize] FAIL — corner drag did not request a resize"; echo "--- press/geom lines (DDR-937) ---"; grep -aE 'PRADYOS_WM_GEOM|PRADYOS_MOUSE_OK|PRADYOS_DRAG_START|PRADYOS_RESIZE_REQ|PRADYOS_WM_(MIN|MAX|UNMAX|CLOSE)|PRADYOS_EV_RESIZE_OK|PRADYOS_AGENT_TRIGGER' build/evresize.log || echo "(none)"; echo "--- tail 200 ---"; tail -200 build/evresize.log; exit 1; }
	@grep -q PRADYOS_EV_RESIZE_OK build/evresize.log || { echo "[evresize] FAIL — client did not honor the resize"; echo "--- press/geom lines (DDR-937) ---"; grep -aE 'PRADYOS_WM_GEOM|PRADYOS_MOUSE_OK|PRADYOS_DRAG_START|PRADYOS_RESIZE_REQ|PRADYOS_WM_(MIN|MAX|UNMAX|CLOSE)|PRADYOS_EV_RESIZE_OK|PRADYOS_AGENT_TRIGGER' build/evresize.log || echo "(none)"; echo "--- tail 200 ---"; tail -200 build/evresize.log; exit 1; }
	@echo "[evresize] PASS — $$(grep -a PRADYOS_EV_RESIZE_OK build/evresize.log | head -1)"

# Layer-7 backdrop gate (DDR-716): the settled per-ambiance backdrops (DAY mesh
# nodes, DUSK sun-bloom, NIGHT nebulas) render on the demo cycle's settled
# frames; the compositor announces each ambiance's first settled backdrop and
# PRADYOS_BACKDROP_OK once all four are seen. Client-driven; needs the GPU.
smoke-backdrop: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_BACKDROP DAY\nPRADYOS_BACKDROP DUSK\nPRADYOS_BACKDROP NIGHT\nPRADYOS_BACKDROP_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 ambiance gate (DDR-709): the compositor demo-cycles the 4 sun-driven
# ambiances (DAWN/DAY/DUSK/NIGHT) with OKLab colour transitions, then settles on
# the time-of-day ambiance from the RTC (SYS_CLOCK). Needs the GPU.
smoke-ambiance: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_AMBIANCE DAWN\nPRADYOS_AMBIANCE DAY\nPRADYOS_AMBIANCE DUSK\nPRADYOS_AMBIANCE NIGHT\nPRADYOS_AMBIANCE_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 z-order/focus/input-routing gate (DDR-708): the client creates two
# overlapping surfaces and raises B (top + focused); the compositor composites in
# z-order and reports the focus, then forwards an injected key to the focused
# window, whose client prints PRADYOS_FOCUS_KEY. Needs the GPU + HMP monitor.
smoke-focus: $(IMG) fat-image sfs-image
	@echo "[focus] z-order + focus + key-routing gate (GPU + sendkey -> focused window)..."
	@rm -f build/focus.log /tmp/pfocus.sock
	@bash tools/qemu_runner/input_inject.sh build/focus.log /tmp/pfocus.sock PRADYOS_FOCUS "f" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/pfocus.sock,server,nowait \
	    -serial file:build/focus.log -display none -no-reboot || true
	@grep -q PRADYOS_ZORDER build/focus.log || { echo "[focus] FAIL — no z-order composite"; tail -20 build/focus.log; exit 1; }
	@grep -q "PRADYOS_FOCUS id=" build/focus.log || { echo "[focus] FAIL — no focused window"; tail -20 build/focus.log; exit 1; }
	@grep -q PRADYOS_FOCUS_KEY build/focus.log || { echo "[focus] FAIL — key not routed to focused window"; tail -20 build/focus.log; exit 1; }
	@echo "[focus] PASS — $$(grep -a PRADYOS_FOCUS_KEY build/focus.log | head -1)"

# Reschedule-IPI gate (DDR-SMP-rq-3): a cross-CPU unblock kicks an idle AP
# (directed wake IPI) so it steals the thread promptly, not on its next tick.
smoke-resched: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] resched OK')" \
	FORBIDDEN_SENTINEL="resched FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Font gate (DDR-728): window titles render in the Inter 16px alpha atlas.
smoke-font: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_FONT_OK\nPRADYOS_TITLE_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Motion gate (DDR-727): the mode toggle's damped-spring pulse (sendkey s).
smoke-motion: $(IMG) fat-image sfs-image
	@echo "[motion] spring gate (GPU + sendkey s -> SPRING_OK)..."
	@rm -f build/motion.log /tmp/pmotion.sock
	@bash tools/qemu_runner/input_inject.sh build/motion.log /tmp/pmotion.sock PRADYOS_FOCUS "s" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/pmotion.sock,server,nowait \
	    -serial file:build/motion.log -display none -no-reboot || true
	@grep -q PRADYOS_SPRING_OK build/motion.log || { echo "[motion] FAIL — no spring pulse"; tail -20 build/motion.log; exit 1; }
	@echo "[motion] PASS — spring toggle pulse"

# Cadence gate (DDR-726): the 'k' hotkey shrinks the auto-ambiance cadence so
# a full automatic cycle (pre-transition pulse + 4 advances) proves in seconds.
smoke-cadence: $(IMG) fat-image sfs-image
	@echo "[cadence] auto-ambiance gate (GPU + sendkey k -> test cadence)..."
	@rm -f build/cadence.log /tmp/pcadence.sock
	@bash tools/qemu_runner/input_inject.sh build/cadence.log /tmp/pcadence.sock PRADYOS_FOCUS "k" &
	@# DDR-965: 120 -> 180. The failing capture showed boot+arm consuming ~65 s of
	@# the old window ([hb] t=6500 immediately precedes CAD_ADV n=1), leaving ~55 s
	@# for four advances that needed ~51-57 s — it missed by seconds. The animation
	@# shrink alone does not close that; this does.
	@timeout 180 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/pcadence.sock,server,nowait \
	    -serial file:build/cadence.log -display none -no-reboot || true
	@grep -q PRADYOS_CADENCE_TEST build/cadence.log || { echo "[cadence] FAIL — test knob not armed"; tail -20 build/cadence.log; exit 1; }
	@grep -q PRADYOS_PRETRANSITION build/cadence.log || { echo "[cadence] FAIL — no pre-transition pulse"; tail -20 build/cadence.log; exit 1; }
	@grep -q PRADYOS_CADENCE_OK build/cadence.log || { echo "[cadence] FAIL — no full auto cycle"; tail -20 build/cadence.log; exit 1; }
	@echo "[cadence] PASS — full automatic ambiance cycle at test cadence"

# Scroll gate (DDR-725): QMP wheel events -> virtio-tablet EV_REL/REL_WHEEL ->
# SYS_MOUSE_POLL wheel field -> compositor routes a type-2 surface event to the
# focused window -> the client acks.
smoke-scroll: $(IMG) fat-image sfs-image
	@echo "[scroll] wheel gate: boot(GPU+tablet) + QMP wheel -> focused window..."
	@rm -f build/scroll.log /tmp/pscroll.sock
	@bash tools/qemu_runner/wheel_inject.sh build/scroll.log /tmp/pscroll.sock PRADYOS_FOCUS &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pscroll.sock,server,nowait \
	    -serial file:build/scroll.log -display none -no-reboot || true
	@grep -q "PRADYOS_SCROLL d=" build/scroll.log || { echo "[scroll] FAIL — compositor saw no wheel"; tail -20 build/scroll.log; exit 1; }
	@grep -q "PRADYOS_EV_SCROLL_OK" build/scroll.log || { echo "[scroll] FAIL — scroll did not reach the focused client"; tail -20 build/scroll.log; exit 1; }
	@echo "[scroll] PASS — $$(grep -a PRADYOS_EV_SCROLL_OK build/scroll.log | head -1)"

# Decorations gate (DDR-724): windows carry a 1px frame (accent when focused,
# gray otherwise) + a fading right/bottom drop shadow.
smoke-decor: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_DECOR_OK\nPRADYOS_SURFACE_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Gradient gate (DDR-723): the backdrop base is a 3-stop vertical gradient
# derived from the ambiance bg (horizon lightening, floor darkening).
smoke-gradient: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_GRADIENT_OK\nPRADYOS_BACKDROP_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Glass-blur gate (DDR-722): glass cards blur + saturate the scene beneath them
# (separable box blur) before the tint; sentinel on the first blurred card.
smoke-glassblur: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_GLASS_BLUR_OK\nPRADYOS_GLASS_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Page-flip gate (DDR-721): two host GPU resources over one guest buffer; every
# flush transfers into the off-screen one and flips scanout — the sentinel
# prints once both resources have been presented (tear-free by construction).
smoke-flip: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf '[gpu] page-flip OK\nPRADYOS_COMPOSITOR_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Window-cycling gate (DDR-720): Tab is a compositor hotkey — each press raises
# the bottom-most visible window (focus + top). Two Tabs must cycle two
# DIFFERENT windows (A and B swap as each raise buries the other).
smoke-alttab: $(IMG) fat-image sfs-image
	@echo "[alttab] Tab window-cycling gate (GPU + sendkey tab -> WM_CYCLE)..."
	@rm -f build/alttab.log /tmp/palttab.sock
	@bash tools/qemu_runner/input_inject.sh build/alttab.log /tmp/palttab.sock PRADYOS_FOCUS "tab" &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci \
	    -monitor unix:/tmp/palttab.sock,server,nowait \
	    -serial file:build/alttab.log -display none -no-reboot || true
	@n=$$(grep -ac PRADYOS_WM_CYCLE build/alttab.log || true); \
	 d=$$(grep -ao 'PRADYOS_WM_CYCLE id=[0-9]*' build/alttab.log | sort -u | wc -l); \
	 [ "$$n" -ge 2 ] || { echo "[alttab] FAIL — fewer than 2 cycles ($$n)"; tail -20 build/alttab.log; exit 1; }; \
	 [ "$$d" -ge 2 ] || { echo "[alttab] FAIL — cycling did not rotate windows"; tail -20 build/alttab.log; exit 1; }; \
	 echo "[alttab] PASS — $$n cycles over $$d windows"

# Layer-7 named-agent panel gate (DDR-707): the compositor renders the 8 named
# agent cards and reports the roster; the AETHER daemon's spawn lights KRYOS
# (active), the rest inactive — state tied to AETHER's roster. Needs the GPU.
# TIMEOUT_S=150: the KRYOS 'active' line needs the AETHER daemon's spawn, which
# lands late under loaded KVM-less CI runners — this gate flaked twice on GitHub
# at 90 s (runs 28341424605, 28614622428) while always passing locally.
smoke-agents: $(IMG) fat-image sfs-image
	TIMEOUT_S=150 QEMU_GPU=1 EXTRA_SENTINEL="$$(printf 'PRADYOS_AGENTS_OK\nAGENT KRYOS active\nAGENT SOLIN inactive')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 per-agent live-metrics gate (DDR-730/735): the agentmetricstest probe
# asserts POST-MORTEM-STABLE facts — slot 0 (KRYOS) has pid!=0 AND dispatches>=1
# (spawned + provably scheduled; the kernel captures the final counters at exit,
# so this holds during OR after the agent's life) while slot 7 (SOLIN) stays
# pid==0/dispatches==0. The original alive-window assertion (state>=1) was RACY
# on TCG CI runners — an agent's whole life fits between two probe samples when
# a compositor quantum takes seconds — and is now printed opportunistically, not
# required. No GPU needed; generous timeout for the daemon's late spawn on CI.
smoke-agentmetrics: $(IMG) fat-image sfs-image
	TIMEOUT_S=150 \
	EXTRA_SENTINEL="$$(printf 'AGENT_METRIC KRYOS sched ok\nPRADYOS_AGENT_METRICS_OK')" \
	FORBIDDEN_SENTINEL="AGENT_METRICS FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer-7 agent-panel metrics gate (DDR-737): the compositor renders the agent
# cards from SYS_AGENT_METRICS (state-colored dot + activity pips) and prints a
# one-shot AGENT_PANEL witness keyed on the post-mortem-stable fact (pid
# retained + dispatches captured at exit, DDR-735) — deterministic regardless of
# frame cadence on TCG. Needs the GPU (the compositor is the witness).
smoke-agentpanel: $(IMG) fat-image sfs-image
	TIMEOUT_S=150 QEMU_GPU=1 \
	EXTRA_SENTINEL="$$(printf 'AGENT_PANEL KRYOS act=\nPRADYOS_AGENT_PANEL_METRICS_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER boot-config gate (DDR-732/761): the daemon reads /etc/aether/config off
# the SFS root (kernel-provisioned, DDR-760/761) and applies mode/task/slot from
# it — the CFG_OK line proves the file was read AND parsed (CFG_DEFAULT, the compiled
# fallback, is the forbidden pattern); AGENT_DONE proves the configured spawn ran.
smoke-aethercfg: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_AETHER_CFG_OK mode=sovereign task=test slot=0\nPRADYOS_AGENT_DONE')" \
	FORBIDDEN_SENTINEL="PRADYOS_AETHER_CFG_DEFAULT" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# CAP_NET egress-allowlist gate (DDR-734): the kernel match/deny self-test
# proves exact-host+port allow, wrong-port deny, wrong-host deny; the daemon's
# NET_ALLOW_OK n=1 proves the /etc/aether/config net= row travelled config -> parser ->
# sovereign-only SYS_NET_ALLOW -> kernel list. Deny-by-default for agents.
smoke-netallow: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf '[net] allowlist OK\nPRADYOS_NET_ALLOW_OK n=1')" \
	FORBIDDEN_SENTINEL="allowlist FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# CAP_NET socket-authority gate (DDR-731): a CAP-less probe must get -EPERM from
# SYS_SOCK_CONNECT (authority) and from WRITE/CLOSE on a slot it doesn't own
# (per-slot ownership) — proving arbitrary processes can no longer reach the
# network or hijack another process's connection. Kernel-side lwIP unaffected.
smoke-capnet: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'CAPNET_CONNECT_DENIED\nCAPNET_SLOT_DENIED\nPRADYOS_CAPNET_OK')" \
	FORBIDDEN_SENTINEL="CAPNET FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Layer 7 mode-binding gate (DDR-701): the daemon (CAP_SOVEREIGN) toggles the
# Sovereign/Manual mode both ways via SYS_SET_MODE and confirms via SYS_GET_MODE,
# proving the toggle's control path end-to-end (the brief's Super+M toggle is a
# renderer over this binding). Ends in SOVEREIGN so smoke-aether is unaffected.
smoke-mode: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'PRADYOS_MODE_SOVEREIGN\nPRADYOS_MODE_MANUAL\nPRADYOS_MODE_TOGGLE_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER LIVE agent gate (ADR-027 §D6): DEVELOPER-RUN, not CI — needs a real
# Ollama. Rebuilds the agent with AETHER_TEST_MODE=0 (host defaults to 10.0.2.2 =
# the QEMU SLIRP gateway, i.e. the developer's loopback) and boots; the agent
# connects over the proxy-socket NSI, POSTs /api/generate, and prints
# PRADYOS_AGENT_LIVE_OK. Usage: make smoke-agent-live [OLLAMA_HOST=a.b.c.d]
OLLAMA_HOST ?= 10.0.2.2
smoke-agent-live: $(STAGE1_BIN) $(STAGE2_BIN) fat-image sfs-image
	@rm -f build/agent_base.o build/agent_base.elf build/kernel.bin build/pradyos.img
	@hexbe=$$(echo $(OLLAMA_HOST) | awk -F. '{printf "0x%02X%02X%02X%02X",$$1,$$2,$$3,$$4}'); \
	  echo "[agent-live] building agent for live Ollama at $(OLLAMA_HOST):11434 (BE $$hexbe)"; \
	  $(MAKE) image USER_AGENT_DEFS="-DAETHER_TEST_MODE=0 -DOLLAMA_HOST_BE=$$hexbe"
	TIMEOUT_S=120 EXTRA_SENTINEL=PRADYOS_AGENT_LIVE_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER security gate (ADR-026 §security): every bound is exercised in-boot —
# queue overflow -> -EAGAIN, audit ring wrap, per-agent OOM kill, syscall
# rate-limit kill, and the no-self-escalation mem-cap rejection. No panic.
smoke-aether-sec: $(IMG) fat-image sfs-image
	TIMEOUT_S=120 EXTRA_SENTINEL="$$(printf 'AETHER_SEC_QUEUE_FULL\nAETHER_AUDIT_WRAP\nAGENT_OOM_KILLED\nAGENT_RATE_LIMITED\nAETHER_SEC_CAP_DENIED\nPRADYOS_AETHER_SEC_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# PROC-A pipe/dup2 gate: systest pipe()s, round-trips "PIPE" through the ring, and
# dup2's the read end onto fd 30 and round-trips again.
smoke-syspipe: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'PIPE: roundtrip OK\nPIPE: dup2 OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# PROC-B epoll gate: systest watches a pipe read-end (EPOLLIN); empty -> 0 ready,
# after a write -> 1 ready event.
smoke-sysepoll: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL='EPOLL: pipe event OK' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# PROC-C signal gate: systest installs a SIGUSR1 handler, kills itself, and busy-
# loops; a timer IRQ delivers the signal, the handler prints, sigreturn resumes.
smoke-syssignal: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL='SIGNAL: SIGUSR1 caught' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# PROC-E io_uring gate: systest mmaps a ring, submits a batched WRITE then READ on
# a pipe in one io_uring_enter, and verifies both completions + the data.
smoke-sysiouring: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL='IO_URING: batch read OK' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

clean:
	rm -rf build
	cd $(TC_DIR)/hello_rs && cargo clean 2>/dev/null || true

# DDR-821 SHA-512 vector gate (FIPS 180-4). Four vectors; the 112-byte case is
# the only one whose message ends exactly where the 128-bit length field goes
# (112 = 128-16), forcing the two-block path, and the 1M-'a' case is streamed in
# 1000-byte chunks so the partial-block carry actually runs.
smoke-sha512: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=sha512 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SHA512_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_SHA512_STUB\nSHA512 FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-819 ChaCha20-Poly1305 vector gate (RFC 8439). Five checks: the §2.4.2
# keystream isolates the cipher, the §2.5.2 tag isolates the authenticator with
# a 34-byte message (2-byte final block — the only path that catches the
# short-block defect DDR-819 actually shipped and caught), a seal/open
# round-trip that depends on no published constant, and two distinct rejection
# arms (tampered ciphertext, tampered tag).
#
# NOT asserted: constant-time tag comparison. A constant-time compare and an
# early-exit memcmp reject the same inputs; QEMU cannot distinguish them.
smoke-aead: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_PROBES=aead \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_AEAD_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_AEAD_STUB\nAEAD FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-821 Ed25519 vector gate (RFC 8032 §7.1). TIMEOUT_S=150: the probe runs 8
# scalar multiplications plus a 1023-byte hash under TCG, which is materially
# heavier than any other crypto gate. Measured, not guessed — see DDR-821.
smoke-ed25519: $(IMG) fat-image sfs-image
	TIMEOUT_S=150 QEMU_PROBES=ed25519 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ED25519_VECTORS_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_ED25519_STUB\nED25519 FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-813 ACC envelope gate. Four arms, all previously proven on the host:
# round-trip, tampered ciphertext (AEAD tag) AND tampered signature (Ed25519) as
# separate arms, replay, and owner-read-after-reboot with no session state —
# the last is what BUG-1 exists for and the only arm that catches a verify key
# kept outside the envelope.
#
# TIMEOUT_S=150: the probe performs ~6 X25519 scalarmults and ~8 Ed25519 ones
# under TCG. Chosen by analogy with smoke-ed25519 (measured), not guessed, and
# DDR-828's rule applies — check elapsed against the window before blaming code.
smoke-acc: $(IMG) fat-image sfs-image
	TIMEOUT_S=150 QEMU_PROBES=acc \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ACC_OK')" \
	FORBIDDEN_SENTINEL="$$(printf 'PRADYOS_ACC_STUB\nACC FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)
