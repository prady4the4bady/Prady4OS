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
KERNEL_ASMS := arch/x86_64/boot.asm arch/x86_64/cpu.asm arch/x86_64/isr.asm \
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
USER_C_LD      := user/user_c.ld
# user-C compile flags: -mcmodel=large (the 0x8000000000 base exceeds 32-bit
# relocs), musl public headers + our generated bits/. OUR code -> -Werror.
USER_C_CFLAGS  := --target=x86_64-elf -ffreestanding -fno-pic -fno-pie -mcmodel=large \
  -nostdinc -nostdlib -Wall -Wextra -Werror \
  -Ibuild/musl/include -I$(MUSL_DIR)/arch/x86_64 -I$(MUSL_DIR)/arch/generic -I$(MUSL_DIR)/include
KERNEL_CS   := kernel/main.c kernel/console.c kernel/idt.c kernel/irq.c \
               kernel/mm/pmm.c kernel/mm/kheap.c kernel/mm/vmm.c kernel/mm/vmm_cow.c kernel/mm/uaccess.c kernel/cap.c \
               kernel/proc/sched.c kernel/proc/tss.c kernel/proc/fd.c kernel/proc/pipe.c kernel/proc/epoll.c kernel/proc/signal.c kernel/ipc/ipc.c \
               kernel/ipc/bcast.c kernel/syscall/syscall.c kernel/syscall/sys_io.c kernel/syscall/sys_file.c kernel/syscall/sys_proc.c kernel/syscall/sys_mmap.c kernel/syscall/sys_exec.c kernel/syscall/sys_fork.c kernel/syscall/sys_wait.c kernel/syscall/sys_io_uring.c kernel/acpi/acpi.c \
               kernel/drivers/pcie/pcie.c kernel/drivers/virtio/virtio_ring.c \
               kernel/drivers/virtio/virtio.c kernel/drivers/virtio/virtio_pci.c \
               kernel/drivers/blk/blk.c kernel/drivers/blk/virtio_blk.c \
               kernel/drivers/net/virtio_net.c kernel/drivers/net/netbuf.c \
               kernel/drivers/gpu/virtio_gpu.c \
               kernel/drivers/input/ps2kbd.c kernel/drivers/input/virtio_input.c \
               kernel/drivers/rtc/rtc.c \
               kernel/fs/vfs/vfs.c kernel/fs/fat32/fat32.c kernel/fs/sfs/sfs.c \
               kernel/fs/sfs/lz4.c kernel/fs/ext4/ext4.c kernel/exec/elf.c kernel/string.c \
               kernel/arch/x86_64/cpu_mitigations.c kernel/vdso/vdso_page.c \
               kernel/aether/aether.c kernel/aether/aether_queue.c kernel/aether/aether_audit.c kernel/aether/aether_mem.c kernel/syscall/sys_aether.c kernel/syscall/sys_socket.c kernel/syscall/sys_fb.c kernel/syscall/sys_surface.c \
               kernel/apic/lapic.c kernel/apic/smp.c kernel/apic/percpu.c
KERNEL_LD   := kernel/kernel.ld
KERNEL_ELF  := build/kernel.elf
KERNEL_BIN  := build/kernel.bin
# boot.o MUST be first so kernel_entry (.text.boot) lands at the image start.
KERNEL_OBJS := build/boot.o build/cpu.o build/isr.o build/context.o \
               build/syscall_entry.o build/usermode.o build/main.o \
               build/console.o build/idt.o build/irq.o build/pmm.o build/kheap.o \
               build/vmm.o build/vmm_cow.o build/uaccess.o build/cap.o build/sched.o build/tss.o build/fd.o build/pipe.o build/epoll.o build/signal.o build/ipc.o \
               build/bcast.o build/syscall.o build/sys_io.o build/sys_file.o build/sys_proc.o build/sys_mmap.o build/sys_exec.o build/sys_fork.o build/sys_wait.o build/sys_io_uring.o build/acpi.o build/pcie.o \
               build/virtio_ring.o build/virtio.o build/virtio_pci.o build/blk.o \
               build/virtio_blk.o build/virtio_net.o build/netbuf.o build/virtio_gpu.o build/rtc.o build/vfs.o build/fat32.o build/sfs.o build/lz4.o \
               build/ext4.o build/elf.o build/user_image.o build/string.o build/cpu_mitigations.o build/vdso_page.o \
               build/aether.o build/aether_queue.o build/aether_audit.o build/aether_mem.o build/sys_aether.o build/sys_socket.o build/sys_fb.o build/sys_input.o build/ps2kbd.o build/virtio_input.o build/sys_surface.o \
               build/lwip_port.o build/lapic.o build/smp.o build/percpu.o build/ap_boot.o
# Kernel include search paths (so "#include "pmm.h"" resolves after the
# kernel/ subdirectory reorganization).
KINCLUDES   := -Ikernel -Ikernel/mm -Ikernel/proc -Ikernel/ipc -Ikernel/syscall \
               -Ikernel/acpi -Ikernel/drivers/pcie -Ikernel/drivers/virtio -Ikernel/drivers/rtc \
               -Ikernel/drivers/blk -Ikernel/drivers/net -Ikernel/drivers/gpu -Ikernel/drivers/input -Ikernel/fs/vfs -Ikernel/fs/fat32 -Ikernel/fs/sfs \
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
# Treat every assembler warning as fatal too (user mandate: zero warnings).
NASM_WERROR := -Werror

.PHONY: all setup toolchain-check kernel musl lwip image smoke smoke-fpu smoke-init smoke-shell smoke-fs smoke-fs-rw smoke-fs-sfs-rw smoke-fs-ext4 smoke-user smoke-uaccess smoke-sysio smoke-sysfile smoke-sysproc smoke-sysmmap smoke-sysexec smoke-sysfork smoke-syswait smoke-mitigations smoke-pmm-poison smoke-vdso smoke-cowfork smoke-net smoke-net-lo smoke-net-fuzz smoke-aether smoke-aether-queue smoke-aether-sec smoke-agent-live smoke-mode smoke-gpu smoke-fb smoke-input smoke-compositor smoke-mouse smoke-surface smoke-agents smoke-focus smoke-ambiance smoke-drag smoke-syspipe smoke-sysepoll smoke-syssignal smoke-sysiouring fat-image sfs-image ext4-image clean

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

$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_LD) $(USER_SRC) $(USER_WX_SRC) $(USER_SYS_SRC) $(USER_EXEC_SRC) $(USER_TLS_SRC) $(USER_FPU_SRC) $(USER_CMUSL_SRC) $(USER_INIT_SRC) $(USER_PRISM_SRC) $(USER_AETHERD_SRC) $(USER_AGENT_SRC) $(USER_INPUT_SRC) $(USER_COMP_SRC) $(USER_SURF_SRC) $(USER_SURFDESTROY_SRC) $(USER_AGENTMETRICS_SRC) $(USER_CAPNET_SRC) $(USER_ROOTMNT_SRC) $(USER_C_LD) $(MUSL_LIB) $(MUSL_CRT) $(LWIP_LIB) third_party/lwip-port/lwip_port.c $(USER_LD)
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
	@for e in $(USER_ELF) $(USER_WX_ELF) $(USER_SYS_ELF) $(USER_EXEC_ELF) $(USER_TLS_ELF) $(USER_FPU_ELF) $(USER_CMUSL_ELF) $(USER_INIT_ELF) $(USER_PRISM_ELF) $(USER_AETHERD_ELF) $(USER_AGENT_ELF) $(USER_INPUT_ELF) $(USER_COMP_ELF) $(USER_SURF_ELF) $(USER_SURFDESTROY_ELF) $(USER_AGENTMETRICS_ELF) $(USER_CAPNET_ELF) $(USER_ROOTMNT_ELF) $(USER_FSRM_ELF); do test "$$(wc -c < $$e)" -le 262144 || { echo "$$e exceeds 256 KiB (EXEC_MAX user-ELF budget)"; exit 1; }; done
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/user_image.asm    -o build/user_image.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/boot.asm          -o build/boot.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/cpu.asm           -o build/cpu.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/isr.asm           -o build/isr.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/context.asm       -o build/context.o
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
	$(CC) $(KCFLAGS) -c kernel/apic/smp.c        -o build/smp.o
	$(CC) $(KCFLAGS) -c kernel/apic/percpu.c     -o build/percpu.o
	$(CC) $(KCFLAGS) -c kernel/drivers/pcie/pcie.c           -o build/pcie.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_ring.c  -o build/virtio_ring.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio.c       -o build/virtio.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_pci.c   -o build/virtio_pci.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/blk.c             -o build/blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/virtio_blk.c      -o build/virtio_blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/virtio_net.c     -o build/virtio_net.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/netbuf.c         -o build/netbuf.o
	$(CC) $(KCFLAGS) -c kernel/drivers/gpu/virtio_gpu.c     -o build/virtio_gpu.o
	$(CC) $(KCFLAGS) -c kernel/drivers/input/ps2kbd.c       -o build/ps2kbd.o
	$(CC) $(KCFLAGS) -c kernel/drivers/input/virtio_input.c -o build/virtio_input.o
	$(CC) $(KCFLAGS) -c kernel/drivers/rtc/rtc.c            -o build/rtc.o
	$(CC) $(KCFLAGS) -c kernel/fs/vfs/vfs.c                  -o build/vfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/fat32/fat32.c             -o build/fat32.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/sfs.c                 -o build/sfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/lz4.c                 -o build/lz4.o
	$(CC) $(KCFLAGS) -c kernel/fs/ext4/ext4.c              -o build/ext4.o
	$(CC) $(KCFLAGS) -c kernel/exec/elf.c                  -o build/elf.o
	$(CC) $(KCFLAGS) -c kernel/string.c        -o build/string.o
	$(CC) $(KCFLAGS) -c kernel/arch/x86_64/cpu_mitigations.c -o build/cpu_mitigations.o
	$(CC) $(KCFLAGS) -c kernel/vdso/vdso_page.c -o build/vdso_page.o
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
	@test "$$(wc -c < $(KERNEL_BIN))" -le 786432 || { echo "kernel.bin exceeds 768 KiB — the stage-2 read window (24x64 sectors from LBA 17, DDR-733). Raise the chunk count in boot/stage2/stage2.asm and grow the 1 MiB disk image together (mind the 2 MiB PT_HI runtime ceiling checked next)."; exit 1; }
	@end=$$($(NM) $(KERNEL_ELF) | awk '$$3 == "__bss_end" { print $$1 }'); \
	 phys=$$(( 0x$$end - 0xFFFFFFFF80000000 + 0x400000 )); \
	 test $$phys -le $$(( 0x600000 )) || { echo "kernel image+BSS ends at phys $$phys, past 0x600000 — the 2 MiB PT_HI higher-half span (DDR-733). Extend PT_HI in boot/stage2/stage2.asm (second PT) before growing further."; exit 1; }
	@echo "kernel: $(KERNEL_BIN) ($$(wc -c < $(KERNEL_BIN)) bytes)"

# Lay the three artifacts onto a 1 MiB raw disk at fixed LBAs:
#   LBA 0  stage1 (512 B MBR)   LBA 1  stage2 (<= 16 sectors)   LBA 17  kernel
# Stage 1 loads 16 sectors of Stage 2; Stage 2 bounce-loads 24x64 sectors of the
# kernel from LBA 17 to 4 MiB (DDR-733) — LBAs hard-coded in the asm, match here.
image: $(IMG)

$(IMG): $(STAGE1_SRC) $(STAGE2_SRC) $(KERNEL_BIN)
	@mkdir -p build
	$(NASM) $(NASM_WERROR) -f bin $(STAGE1_SRC) -o $(STAGE1_BIN)
	@test "$$(wc -c < $(STAGE1_BIN))" -eq 512 || { echo "stage1.bin is not 512 bytes (got $$(wc -c < $(STAGE1_BIN)))"; exit 1; }
	$(NASM) $(NASM_WERROR) -f bin $(STAGE2_SRC) -o $(STAGE2_BIN)
	@test "$$(wc -c < $(STAGE2_BIN))" -le 8192 || { echo "stage2.bin exceeds 8 KiB; Stage 1 only loads 16 sectors"; exit 1; }
	truncate -s 1M $(IMG)
	dd if=$(STAGE1_BIN) of=$(IMG) bs=512 seek=0  conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$(IMG) bs=512 seek=1  conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$(IMG) bs=512 seek=17 conv=notrunc status=none
	@echo "image: $(IMG) (stage1 $$(wc -c < $(STAGE1_BIN))B, stage2 $$(wc -c < $(STAGE2_BIN))B, kernel $$(wc -c < $(KERNEL_BIN))B)"

# A FAT32 data disk (with known files) for the VFS/FAT32 self-test. 64 MiB is
# above the FAT32 minimum. Uses dosfstools (mkfs.fat) + mtools (mcopy/mmd).
# `fat-image` is phony so every FS gate starts from a FRESH volume — the kernel
# write test creates /KOUT.TXT, which must not already exist on a second run.
FAT_IMG := build/fat.img

fat-image:
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
	# DDR-732/734: AETHER boot policy — operator-editable, read by the daemon at
	# boot. net= rows are the CAP_NET egress allowlist (deny-by-default); the
	# default entry is the Ollama endpoint at the SLIRP gateway (smoke-agent-live).
	printf 'mode=sovereign\ntask=test\nslot=0\nnet=10.0.2.2:11434\n' > build/aether.cfg
	mcopy -i $(FAT_IMG) build/aether.cfg ::/AETHER.CFG
	@echo "fat: $(FAT_IMG) (FAT32, /HELLO.TXT, /DOCS/NOTE.TXT, /LongFileName.txt, /AETHER.CFG)"

# A blank 16 MiB disk for the SFS self-test; the kernel formats it as SFS in
# place (in-kernel mkfs) then mounts it. Phony so each gate starts blank.
SFS_IMG := build/sfs.img

sfs-image:
	@mkdir -p build
	dd if=/dev/zero of=$(SFS_IMG) bs=1M count=16 status=none
	@echo "sfs: $(SFS_IMG) (16 MiB blank — kernel formats in place)"

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
	bash tools/qemu_runner/boot_test.sh $(IMG)

# Filesystem gate: builds the FAT32 data disk and asserts BOTH the kernel
# sentinel AND the FAT32 read self-test line — real end-to-end FS coverage.
# Needs dosfstools (mkfs.fat) + mtools (mcopy); see setup_toolchain.sh.
smoke-fs: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf 'msix vec=50\nPRADYOS filesystem works!\nnested file ok\nlong name read works\n[rtc] 20\nkernel wrote this\ncreated+deleted /TMP.TXT OK\ncreate/lookup OK\nbyte-exact OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Read-write FS gate with ADVERSARIAL HOST-SIDE VALIDATION: boot the kernel (it
# creates+writes /KOUT.TXT and create+deletes /TMP.TXT on the FAT32 disk), then
# on the host run fsck.fat to prove the volume is still consistent and mdir/mtype
# to prove the kernel-written file persisted with the right contents. QEMU writes
# the modified disk image back to build/fat.img, so the host sees the kernel's
# changes. Needs dosfstools (fsck.fat) + mtools (mdir/mtype).
smoke-fs-rw: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf 'kernel wrote this\ncreated+deleted /TMP.TXT OK')" \
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
	EXTRA_SENTINEL="$$(printf 'create/lookup OK\nbyte-exact OK\nto 69632 OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SFS hierarchical-directory gate (DDR-738): the kernel builds /etc/aether/config
# (mkdir -p intermediates), reads it back from a fresh path walk, rejects opening
# a directory as a file and a missing intermediate, and enumerates each level.
smoke-sfs-dirs: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf '[sfs] hier dirs OK')" \
	FORBIDDEN_SENTINEL="hier dirs FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SFS unlink/rmdir gate (DDR-741): the kernel removes a file (tombstone),
# re-creates the freed name, refuses rmdir on a non-empty dir, removes it
# leaf-first, and confirms readdir no longer lists it.
smoke-sfs-unlink: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf '[sfs] unlink/rmdir OK')" \
	FORBIDDEN_SENTINEL="unlink/rmdir FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# ext4 read-only gate: a host-built ext4 disk with /EXT4.TXT; the kernel mounts
# it (4th disk) and reads the file back. Asserts the ext4 self-test line.
smoke-fs-ext4: $(IMG) fat-image sfs-image ext4-image
	EXTRA_SENTINEL='ext4 read works' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-process root-mount gate (DDR-739): with the ext4 disk attached, a ring-3
# probe is spawned with its root_mnt set to ext4 (not the FAT default). It opens
# /EXT4.TXT (ext4-only) AND fails /HELLO.TXT (FAT-only) -> proves SYS_OPEN
# resolves against the SELECTED root. Needs the ext4 disk (like smoke-fs-ext4).
smoke-rootmount: $(IMG) fat-image sfs-image ext4-image
	EXTRA_SENTINEL="$$(printf 'PRADYOS_ROOTMOUNT_OK ext4')" \
	FORBIDDEN_SENTINEL="ROOTMOUNT FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# DDR-744 ring-3 file-lifecycle gate: the SFS-rooted fsrmtest probe creates a
# file via SYS_OPEN(O_CREAT), reads it back, SYS_UNLINKs it, confirms it is gone,
# and re-unlinks (clean error). Proves O_CREAT + SYS_UNLINK across the syscall
# boundary against the writable SFS root. PRADYOS_FSRM_OK on all-pass.
smoke-fsrm: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf 'PRADYOS_FSRM_OK')" \
	FORBIDDEN_SENTINEL="FSRM FAIL" \
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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf '[user] ELF loaded from SFS; ring-3 thread spawned\nHELLO FROM RING-3\n[user] sys_exit(0)\n[trap] user #PF page fault\n[sfs] lz4+tags compress/readback/tag OK\nPRADYOS_TLS_OK WRITEV_OK\nPRADYOS_MUSL_OK')" \
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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf 'PRADYOS_INIT_OK\ninit: reaped PID=')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# 5e PRISM shell gate (ADR-024 §D6): boot with -serial stdio, pipe a command
# script (echo / help / exit) into the guest UART, and assert the shell came up
# and ran the builtins with no kernel panic. Self-contained (boot_test.sh is
# output-only). FAT32 carries /PRISM.ELF (init execve's it); SFS carries init.
smoke-shell: $(IMG) fat-image sfs-image
	@echo "[shell] booting PRISM; feeding commands once the prompt appears..."
	@# The FIFO must live on a real Linux fs — DrvFs (/mnt/c, where build/ is) does
	@# not support FIFOs. The serial log stays in build/.
	@SHIN=$$(mktemp -u /tmp/pradyos_prism.XXXXXX); rm -f build/shell_serial.log; mkfifo "$$SHIN"; \
	( exec > "$$SHIN"; \
	  for i in $$(seq 1 300); do grep -q PRISM_READY build/shell_serial.log 2>/dev/null && break; sleep 0.1; done; \
	  printf 'echo prism-echo-marker\n'; sleep 0.5; printf 'help\n'; sleep 0.5; printf 'ls /\n'; sleep 0.5; printf 'ps\n'; sleep 0.5; printf 'exit\n'; sleep 0.5 ) & \
	timeout 60 qemu-system-x86_64 -M q35 \
	    -drive if=none,format=raw,file=$(IMG),id=disk0 -device virtio-blk-pci,drive=disk0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=disk1 -device virtio-blk-pci,drive=disk1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=disk2 -device virtio-blk-pci,drive=disk2 \
	    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	    -serial stdio -display none -monitor none < "$$SHIN" > build/shell_serial.log 2>/dev/null || true; \
	rm -f "$$SHIN"
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
	@grep -qE "PID +PPID S U NAME$$" build/shell_serial.log || { echo "[shell] FAIL: ps builtin (DDR-743)"; tail -30 build/shell_serial.log; exit 1; }
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/shell_serial.log; then echo "[shell] FAIL: kernel panic"; tail -30 build/shell_serial.log; exit 1; fi
	@echo "[shell] PASS — PRISM_READY + prompt + echo + help + ls + ps, clean, no panic."

# Phase 5b slice 2 user-access gate: the in-kernel uaccess self-test (main.c)
# drives copyin/copyout/copyinstr against a throwaway user AS — a good page, a
# wild pointer (-> EFAULT), a read-only page write (-> EFAULT, W^X), and a valid
# string. All four lines must appear AND the kernel must survive the two faults.
smoke-uaccess: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL="$$(printf '[uaccess] copyin good page OK\n[uaccess] copyin bad ptr EFAULT OK\n[uaccess] copyout RO page EFAULT OK\n[uaccess] copyinstr OK')" \
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
smoke-sysmmap: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 EXTRA_SENTINEL="$$(printf 'SYSMMAP OK\nSYSMMAP WX REJECTED\nSYSMUNMAP OK')" \
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
	EXTRA_SENTINEL='[cpu] mitigations:' \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# IMP-B poison gate: with KASAN=1 (the default) the kernel poisons freed PMM
# frames and arms slab canaries. The gate asserts the "[pmm] poison enabled"
# banner; because KASAN is the default, every other gate is implicitly a poison /
# canary regression test too (a smashed canary -> KHEAP PANIC -> missing sentinel).
smoke-pmm-poison: $(IMG) fat-image sfs-image
	EXTRA_SENTINEL='[pmm] poison enabled' \
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
	EXTRA_SENTINEL='[vmm] COW fork copy-on-write OK' \
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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf 'msix vec=54\n[net] lwIP up 10.0.2.15/24\nPRADYOS_NET_LO_OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# NET-B fuzz/hardening gate (ADR-025 §D6/§D10): at boot the kernel feeds 512
# malformed/truncated frames and a 256-segment SYN flood (to a closed port)
# straight into the lwIP receive path. Passing = the kernel survives and prints
# the sentinel; boot_test.sh already fails the run on any panic string.
smoke-net-fuzz: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL=PRADYOS_NET_FUZZ_OK \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AETHER queue gate (ADR-026 §D2/§D3): the in-boot self-test submits an action,
# sovereign mode auto-approves it, and an audit entry is written.
smoke-aether-queue: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL=PRADYOS_AETHER_QUEUE_OK \
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
	TIMEOUT_S=60 QEMU_GPU=1 EXTRA_SENTINEL=PRADYOS_GPU_FB_OK \
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
	@echo "[comp] PASS — desktop rendered + keyboard-driven mode flip confirmed."

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
	@awk '/PRADYOS_AGENT_TRIGGER name=PRAX/{t=1} t&&/PRADYOS_AGENT_DONE/{ok=1} END{exit !ok}' build/aclick.log || { echo "[aclick] FAIL — the clicked PRAX agent did not run to completion"; tail -20 build/aclick.log; exit 1; }
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
	@bash tools/qemu_runner/drag_inject.sh build/drag.log /tmp/pdrag.sock PRADYOS_AMBIANCE_OK &
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

# Layer-7 surface-destroy gate (DDR-729, -smp 4 so the exit-reap runs cross-CPU):
# surfdestroytest proves the 16-slot table reclaims on churn/close, reuses freed
# slots, and — the lifecycle hole this slice closes — reclaims a child's surfaces
# when it EXITS without SYS_SURFACE_CLOSE. GPU-independent (surfaces are PMM).
smoke-surfdestroy: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_SURFDESTROY_CHURN_OK\nPRADYOS_SURFDESTROY_REUSE_OK\nPRADYOS_SURFDESTROY_EXIT_OK\nPRADYOS_SURFDESTROY_OK')" \
	FORBIDDEN_SENTINEL="SURFDESTROY FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# APIC stage-A gate (DDR-714): the MADT-discovered LAPIC comes up and the APIC
# timer takes over the 100 Hz tick (PIT masked); every later sentinel in the
# boot (scheduler, FS, user) implicitly proves the new tick drives the system.
smoke-apic: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf '[apic] up id=\n[apic] timer 100Hz (PIT masked)')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SMP stage-B gate (ADR-029): boot 4 vCPUs; the BSP INIT-SIPIs the 3 APs through
# the 0x8000 trampoline into long mode; each announces and parks. Proves the
# MP-init protocol end-to-end (MADT ids -> IPIs -> real->long mode -> C).
smoke-smp: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 tss OK\n[smp] cpu 2 tss OK\n[smp] cpu 3 tss OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="tss FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# APs-in-scheduler gate (ADR-031 cap-2b): the BSP spawns several READY kernel
# probe threads and kicks the APs; a probe reports a non-BSP cpu_idx, proving a
# ready-ring thread executed on an AP (not just a directed mailbox job).
smoke-smpsched: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] sched cross-CPU OK')" \
	FORBIDDEN_SENTINEL="cross-CPU FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AP preemption gate (ADR-031 cap-3): each AP arms its own LAPIC timer, so a
# non-BSP CPU's per-CPU tick counter advances — proving timer-driven preemption
# on APs (under cap-2b it would stay 0).
smoke-smppreempt: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] ap preempt OK')" \
	FORBIDDEN_SENTINEL="ap preempt FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# User-on-AP gate (ADR-031 cap-4, the capstone): a ring-3 thread is claimed and
# run by a non-BSP CPU, with the user programs still passing their own sentinels
# (they must run CORRECTLY on APs, not merely run).
smoke-smpuser: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] user on AP OK\nHELLO FROM RING-3\nPRADYOS_MUSL_OK')" \
	FORBIDDEN_SENTINEL="user on AP FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Runqueue stress gate (DDR-SMP-rq-1): 24 kernel threads in 3 waves over the
# per-CPU ready queues (steal + wake spread them); all must complete.
smoke-rqstress: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] rqstress OK')" \
	FORBIDDEN_SENTINEL="rqstress FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Multi-in-flight gate (DDR-BLK-1): two threads keep requests outstanding on
# one disk concurrently (per-request slots; the one-in-flight mutex is gone),
# each round-tripping its own reads cleanly.
smoke-blkmq: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[blk] multi-inflight OK\n[sfs] lz4+tags compress/readback/tag OK')" \
	FORBIDDEN_SENTINEL="multi-inflight FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# MSI-X distribution gate (DDR-714C3): the blk vectors target APs; a disk
# completion handler provably ran on a non-BSP CPU while the FS phase's actual
# I/O all still passes (correctness under cross-CPU completion).
smoke-msixap: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[blk] msix on AP OK\n[sfs] lz4+tags compress/readback/tag OK')" \
	FORBIDDEN_SENTINEL="msix on AP FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SMP locking gate (ADR-030 stage 1): each AP allocates+frees a PMM page and a
# slab object through the new subsystem spinlocks before parking; all three must
# report locks OK (and none FAIL) alongside full bring-up.
smoke-smplock: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 locks OK\n[smp] cpu 2 locks OK\n[smp] cpu 3 locks OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="locks FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-CPU identity gate (ADR-030 stage 2, DDR-SMP-2): the BSP and every AP
# record + round-trip their percpu entry (LAPIC-ID-indexed this_cpu()).
smoke-percpu: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[percpu] bsp idx=\n[smp] cpu 1 percpu OK\n[smp] cpu 2 percpu OK\n[smp] cpu 3 percpu OK')" \
	FORBIDDEN_SENTINEL="percpu FAIL" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# SWAPGS gate (ADR-030 stage 3a, DDR-SMP-3a): the %gs:0 percpu self-pointer must
# round-trip from a ring-3-entered syscall (only true when the SWAPGS discipline
# on syscall/isr/usermode transitions is balanced) AND from every parked AP.
smoke-swapgs: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[percpu] gs OK (syscall ctx)\n[smp] cpu 1 percpu OK\n[smp] cpu 2 percpu OK\n[smp] cpu 3 percpu OK\n[smp] cpus online=4/4')" \
	FORBIDDEN_SENTINEL="$$(printf 'gs FAIL\npercpu FAIL')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# AP work-dispatch gate (ADR-030 stage 3c-alpha, DDR-SMP-3c-alpha): the BSP
# posts a job to each idle AP's mailbox + wake IPI (vector 49); every AP runs
# it and reports, then the BSP confirms all mailboxes drained.
smoke-smpjob: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cpu 1 job OK\n[smp] cpu 2 job OK\n[smp] cpu 3 job OK\n[smp] jobs done=3')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Cross-CPU wake gate (ADR-030 3c-locks-1): a BSP thread blocks; an AP job
# sched_unblocks it (atomic CAS, no ring lock needed for state-only
# transitions); it resumes on the (spinlocked) BSP scheduler.
smoke-crosswake: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
	EXTRA_SENTINEL="$$(printf '[smp] cross-wake waiting\n[smp] cross-wake OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# Per-CPU scheduler-state gate (ADR-030 stage 3b, DDR-SMP-3b): current_thread +
# the SYSCALL kstack now live at %gs:8/%gs:16; the probe verifies the running
# thread resolves through percpu from ring-3 syscall context.
smoke-percpu-sched: $(IMG) fat-image sfs-image
	TIMEOUT_S=60 QEMU_SMP=4 \
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
	@ABSX=16190 ABSY=2602 bash tools/qemu_runner/mouse_inject.sh build/wmclose.log /tmp/pwmclose.sock PRADYOS_AMBIANCE_OK &
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
	@ABSX=5760 ABSY=5588 bash tools/qemu_runner/mouse_inject.sh build/wmmin.log /tmp/pwmmin.sock PRADYOS_AMBIANCE_OK &
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
	@ABSX=5311 ABSY=5588 bash tools/qemu_runner/mouse_inject.sh build/wmmax.log /tmp/pwmmax.sock PRADYOS_AMBIANCE_OK &
	@ABSX=15424 ABSY=725 bash tools/qemu_runner/mouse_inject.sh build/wmmax.log /tmp/pwmmax.sock "PRADYOS_EV_RESIZE_OK w=512" &
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
	@SX=6303 SY=8404 EX=9599 EY=11090 bash tools/qemu_runner/drag_inject.sh build/evresize.log /tmp/pevrs.sock PRADYOS_AMBIANCE_OK &
	@timeout 120 qemu-system-x86_64 -machine q35 \
	    -drive if=none,format=raw,file=$(IMG),id=d0 -device virtio-blk-pci,drive=d0,bootindex=0 \
	    -drive if=none,format=raw,file=$(FAT_IMG),id=d1 -device virtio-blk-pci,drive=d1 \
	    -drive if=none,format=raw,file=$(SFS_IMG),id=d2 -device virtio-blk-pci,drive=d2 \
	    -device virtio-gpu-pci -device virtio-tablet-pci \
	    -qmp unix:/tmp/pevrs.sock,server,nowait \
	    -serial file:build/evresize.log -display none -no-reboot || true
	@grep -q "PRADYOS_RESIZE_REQ id=1" build/evresize.log || { echo "[evresize] FAIL — corner drag did not request a resize"; tail -20 build/evresize.log; exit 1; }
	@grep -q PRADYOS_EV_RESIZE_OK build/evresize.log || { echo "[evresize] FAIL — client did not honor the resize"; tail -20 build/evresize.log; exit 1; }
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
	TIMEOUT_S=60 QEMU_SMP=4 \
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
	@timeout 120 qemu-system-x86_64 -machine q35 \
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

# AETHER boot-config gate (DDR-732): the daemon reads /AETHER.CFG off the FAT32
# boot volume (mcopy'd at image build) and applies mode/task/slot from it — the
# CFG_OK line proves the file was read AND parsed (CFG_DEFAULT, the compiled
# fallback, is the forbidden pattern); AGENT_DONE proves the configured spawn ran.
smoke-aethercfg: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 \
	EXTRA_SENTINEL="$$(printf 'PRADYOS_AETHER_CFG_OK mode=sovereign task=test slot=0\nPRADYOS_AGENT_DONE')" \
	FORBIDDEN_SENTINEL="PRADYOS_AETHER_CFG_DEFAULT" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

# CAP_NET egress-allowlist gate (DDR-734): the kernel match/deny self-test
# proves exact-host+port allow, wrong-port deny, wrong-host deny; the daemon's
# NET_ALLOW_OK n=1 proves the /AETHER.CFG net= row travelled config -> parser ->
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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf 'AETHER_SEC_QUEUE_FULL\nAETHER_AUDIT_WRAP\nAGENT_OOM_KILLED\nAGENT_RATE_LIMITED\nAETHER_SEC_CAP_DENIED\nPRADYOS_AETHER_SEC_OK')" \
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
