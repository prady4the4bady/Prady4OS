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
               arch/x86_64/usermode.asm arch/x86_64/user_image.asm
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
USER_FBTEST_SRC  := user/fbtest.c         # L7: ring-3 framebuffer draw test (DDR-702)
USER_FBTEST_ELF  := build/fbtest.elf
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
               kernel/drivers/rtc/rtc.c \
               kernel/fs/vfs/vfs.c kernel/fs/fat32/fat32.c kernel/fs/sfs/sfs.c \
               kernel/fs/sfs/lz4.c kernel/fs/ext4/ext4.c kernel/exec/elf.c kernel/string.c \
               kernel/arch/x86_64/cpu_mitigations.c kernel/vdso/vdso_page.c \
               kernel/aether/aether.c kernel/aether/aether_queue.c kernel/aether/aether_audit.c kernel/aether/aether_mem.c kernel/syscall/sys_aether.c kernel/syscall/sys_socket.c kernel/syscall/sys_fb.c
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
               build/aether.o build/aether_queue.o build/aether_audit.o build/aether_mem.o build/sys_aether.o build/sys_socket.o build/sys_fb.o \
               build/lwip_port.o
# Kernel include search paths (so "#include "pmm.h"" resolves after the
# kernel/ subdirectory reorganization).
KINCLUDES   := -Ikernel -Ikernel/mm -Ikernel/proc -Ikernel/ipc -Ikernel/syscall \
               -Ikernel/acpi -Ikernel/drivers/pcie -Ikernel/drivers/virtio -Ikernel/drivers/rtc \
               -Ikernel/drivers/blk -Ikernel/drivers/net -Ikernel/drivers/gpu -Ikernel/fs/vfs -Ikernel/fs/fat32 -Ikernel/fs/sfs \
               -Ikernel/fs/ext4 -Ikernel/exec -Ikernel/include -Ikernel/arch/x86_64 -Ikernel/vdso -Ikernel/aether
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

.PHONY: all setup toolchain-check kernel musl lwip image smoke smoke-fpu smoke-init smoke-shell smoke-fs smoke-fs-rw smoke-fs-sfs-rw smoke-fs-ext4 smoke-user smoke-uaccess smoke-sysio smoke-sysfile smoke-sysproc smoke-sysmmap smoke-sysexec smoke-sysfork smoke-syswait smoke-mitigations smoke-pmm-poison smoke-vdso smoke-cowfork smoke-net smoke-net-lo smoke-net-fuzz smoke-aether smoke-aether-queue smoke-aether-sec smoke-agent-live smoke-mode smoke-gpu smoke-fb smoke-syspipe smoke-sysepoll smoke-syssignal smoke-sysiouring fat-image sfs-image ext4-image clean

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

$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_LD) $(USER_SRC) $(USER_WX_SRC) $(USER_SYS_SRC) $(USER_EXEC_SRC) $(USER_TLS_SRC) $(USER_FPU_SRC) $(USER_CMUSL_SRC) $(USER_INIT_SRC) $(USER_PRISM_SRC) $(USER_AETHERD_SRC) $(USER_AGENT_SRC) $(USER_FBTEST_SRC) $(USER_C_LD) $(MUSL_LIB) $(MUSL_CRT) $(LWIP_LIB) third_party/lwip-port/lwip_port.c $(USER_LD)
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
	$(CC) $(USER_C_CFLAGS) -c $(USER_FBTEST_SRC) -o build/fbtest.o
	$(LD) -nostdlib -static -no-pie -T $(USER_C_LD) $(MUSL_CRT) build/fbtest.o $(MUSL_LIB) -o $(USER_FBTEST_ELF)
	@for e in $(USER_ELF) $(USER_WX_ELF) $(USER_SYS_ELF) $(USER_EXEC_ELF) $(USER_TLS_ELF) $(USER_FPU_ELF) $(USER_CMUSL_ELF) $(USER_INIT_ELF) $(USER_PRISM_ELF) $(USER_AETHERD_ELF) $(USER_AGENT_ELF) $(USER_FBTEST_ELF); do test "$$(wc -c < $$e)" -le 262144 || { echo "$$e exceeds 256 KiB (EXEC_MAX user-ELF budget)"; exit 1; }; done
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/user_image.asm    -o build/user_image.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/boot.asm          -o build/boot.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/cpu.asm           -o build/cpu.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/isr.asm           -o build/isr.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/context.asm       -o build/context.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/syscall_entry.asm -o build/syscall_entry.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/usermode.asm      -o build/usermode.o
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
	$(CC) $(KCFLAGS) -c kernel/drivers/pcie/pcie.c           -o build/pcie.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_ring.c  -o build/virtio_ring.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio.c       -o build/virtio.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_pci.c   -o build/virtio_pci.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/blk.c             -o build/blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/virtio_blk.c      -o build/virtio_blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/virtio_net.c     -o build/virtio_net.o
	$(CC) $(KCFLAGS) -c kernel/drivers/net/netbuf.c         -o build/netbuf.o
	$(CC) $(KCFLAGS) -c kernel/drivers/gpu/virtio_gpu.c     -o build/virtio_gpu.o
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
	# NET-B: the lwIP-port glue (first-party, -Werror) — lwIP headers via -isystem
	# (no warnings from them), our shims via -I, -nostdlibinc drops host glibc.
	$(CC) $(KCFLAGS) -nostdlibinc -I$(LWIP_PORT) -isystem $(LWIP_DIR)/src/include -c third_party/lwip-port/lwip_port.c -o build/lwip_port.o
	$(LD) -nostdlib -T $(KERNEL_LD) -o $(KERNEL_ELF) $(KERNEL_OBJS) $(LWIP_LIB)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_BIN)
	@test "$$(wc -c < $(KERNEL_BIN))" -le 524288 || { echo "kernel.bin exceeds 512 KiB; Stage 2 loads 16x64 sectors (page tables at 0x300000; bump the load + 1 MiB image if more is needed)"; exit 1; }
	@echo "kernel: $(KERNEL_BIN) ($$(wc -c < $(KERNEL_BIN)) bytes)"

# Lay the three artifacts onto a 1 MiB raw disk at fixed LBAs:
#   LBA 0  stage1 (512 B MBR)   LBA 1  stage2 (<= 16 sectors)   LBA 17  kernel
# Stage 1 loads 16 sectors of Stage 2; Stage 2 loads 64 sectors of the kernel
# from LBA 17 — both LBAs are hard-coded in the asm and must match here.
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
	@echo "fat: $(FAT_IMG) (FAT32, /HELLO.TXT, /DOCS/NOTE.TXT, /LongFileName.txt)"

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
	EXTRA_SENTINEL="$$(printf 'PRADYOS filesystem works!\nnested file ok\nlong name read works\n[rtc] 20\nkernel wrote this\ncreated+deleted /TMP.TXT OK\ncreate/lookup OK\nbyte-exact OK\njournal abort/commit/replay OK\nversion-isolation OK\ncompress/readback/tag OK')" \
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

# ext4 read-only gate: a host-built ext4 disk with /EXT4.TXT; the kernel mounts
# it (4th disk) and reads the file back. Asserts the ext4 self-test line.
smoke-fs-ext4: $(IMG) fat-image sfs-image ext4-image
	EXTRA_SENTINEL='ext4 read works' \
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
	  printf 'echo prism-echo-marker\n'; sleep 0.5; printf 'help\n'; sleep 0.5; printf 'exit\n'; sleep 0.5 ) & \
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
	@if grep -qiE "\[panic\]|KERNEL PANIC" build/shell_serial.log; then echo "[shell] FAIL: kernel panic"; tail -30 build/shell_serial.log; exit 1; fi
	@echo "[shell] PASS — PRISM_READY + prompt + echo + help, clean, no panic."

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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf '[net] lwIP up 10.0.2.15/24\nPRADYOS_NET_LO_OK')" \
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

# Layer-7 framebuffer-surface gate (DDR-702): a ring-3 program maps the GPU FB
# (SYS_FB_MAP), draws a pattern into it, and presents it (SYS_FB_FLUSH). Proves
# the userspace draw->present path; needs the GPU device, so QEMU_GPU=1.
smoke-fb: $(IMG) fat-image sfs-image
	TIMEOUT_S=90 QEMU_GPU=1 EXTRA_SENTINEL=PRADYOS_FB_DRAW_OK \
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
