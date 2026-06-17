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
               arch/x86_64/context.asm
KERNEL_CS   := kernel/main.c kernel/console.c kernel/idt.c kernel/irq.c \
               kernel/mm/pmm.c kernel/mm/kheap.c kernel/mm/vmm.c kernel/cap.c \
               kernel/proc/sched.c kernel/ipc/ipc.c kernel/ipc/bcast.c \
               kernel/string.c
KERNEL_LD   := kernel/kernel.ld
KERNEL_ELF  := build/kernel.elf
KERNEL_BIN  := build/kernel.bin
# boot.o MUST be first so kernel_entry (.text.boot) lands at the image start.
KERNEL_OBJS := build/boot.o build/cpu.o build/isr.o build/context.o build/main.o \
               build/console.o build/idt.o build/irq.o build/pmm.o build/kheap.o \
               build/vmm.o build/cap.o build/sched.o build/ipc.o build/bcast.o \
               build/string.o
# Kernel include search paths (so "#include "pmm.h"" resolves after the
# kernel/ subdirectory reorganization).
KINCLUDES   := -Ikernel -Ikernel/mm -Ikernel/proc -Ikernel/ipc -Ikernel/syscall
KCFLAGS     := --target=$(X64_TRIPLE) -ffreestanding -fno-pic -fno-pie \
               -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
               -fno-stack-protector -fno-omit-frame-pointer \
               -nostdlib -Wall -Wextra -Werror $(KINCLUDES)
# Treat every assembler warning as fatal too (user mandate: zero warnings).
NASM_WERROR := -Werror

.PHONY: all setup toolchain-check kernel image smoke clean

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

# Build the NEXUS kernel: 64-bit entry stub (NASM) + C main, linked flat at
# 0x10000 and objcopied to a raw binary the bootloader loads verbatim.
kernel: $(KERNEL_BIN)

$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_LD)
	@mkdir -p build
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/boot.asm    -o build/boot.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/cpu.asm     -o build/cpu.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/isr.asm     -o build/isr.o
	$(NASM) $(NASM_WERROR) -f elf64 arch/x86_64/context.asm -o build/context.o
	$(CC) $(KCFLAGS) -c kernel/main.c    -o build/main.o
	$(CC) $(KCFLAGS) -c kernel/console.c -o build/console.o
	$(CC) $(KCFLAGS) -c kernel/idt.c     -o build/idt.o
	$(CC) $(KCFLAGS) -c kernel/irq.c     -o build/irq.o
	$(CC) $(KCFLAGS) -c kernel/mm/pmm.c     -o build/pmm.o
	$(CC) $(KCFLAGS) -c kernel/mm/kheap.c   -o build/kheap.o
	$(CC) $(KCFLAGS) -c kernel/mm/vmm.c     -o build/vmm.o
	$(CC) $(KCFLAGS) -c kernel/cap.c        -o build/cap.o
	$(CC) $(KCFLAGS) -c kernel/proc/sched.c -o build/sched.o
	$(CC) $(KCFLAGS) -c kernel/ipc/ipc.c    -o build/ipc.o
	$(CC) $(KCFLAGS) -c kernel/ipc/bcast.c  -o build/bcast.o
	$(CC) $(KCFLAGS) -c kernel/string.c     -o build/string.o
	$(LD) -nostdlib -T $(KERNEL_LD) -o $(KERNEL_ELF) $(KERNEL_OBJS)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_BIN)
	@test "$$(wc -c < $(KERNEL_BIN))" -le 32768 || { echo "kernel.bin exceeds 32 KiB; Stage 2 only loads 64 sectors"; exit 1; }
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

smoke: $(IMG)
	bash tools/qemu_runner/boot_test.sh $(IMG)

clean:
	rm -rf build
	cd $(TC_DIR)/hello_rs && cargo clean 2>/dev/null || true
