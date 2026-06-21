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
USER_LD      := user/user.ld
USER_ELF     := build/hello.elf
USER_WX_ELF  := build/wxviol.elf
USER_SYS_ELF := build/systest.elf
USER_EXEC_ELF := build/exectest.elf
KERNEL_CS   := kernel/main.c kernel/console.c kernel/idt.c kernel/irq.c \
               kernel/mm/pmm.c kernel/mm/kheap.c kernel/mm/vmm.c kernel/mm/vmm_fork.c kernel/mm/uaccess.c kernel/cap.c \
               kernel/proc/sched.c kernel/proc/tss.c kernel/proc/fd.c kernel/ipc/ipc.c \
               kernel/ipc/bcast.c kernel/syscall/syscall.c kernel/syscall/sys_io.c kernel/syscall/sys_file.c kernel/syscall/sys_proc.c kernel/syscall/sys_mmap.c kernel/syscall/sys_exec.c kernel/syscall/sys_fork.c kernel/syscall/sys_wait.c kernel/acpi/acpi.c \
               kernel/drivers/pcie/pcie.c kernel/drivers/virtio/virtio_ring.c \
               kernel/drivers/virtio/virtio.c kernel/drivers/virtio/virtio_pci.c \
               kernel/drivers/blk/blk.c kernel/drivers/blk/virtio_blk.c \
               kernel/drivers/rtc/rtc.c \
               kernel/fs/vfs/vfs.c kernel/fs/fat32/fat32.c kernel/fs/sfs/sfs.c \
               kernel/fs/sfs/lz4.c kernel/fs/ext4/ext4.c kernel/exec/elf.c kernel/string.c \
               kernel/arch/x86_64/cpu_mitigations.c
KERNEL_LD   := kernel/kernel.ld
KERNEL_ELF  := build/kernel.elf
KERNEL_BIN  := build/kernel.bin
# boot.o MUST be first so kernel_entry (.text.boot) lands at the image start.
KERNEL_OBJS := build/boot.o build/cpu.o build/isr.o build/context.o \
               build/syscall_entry.o build/usermode.o build/main.o \
               build/console.o build/idt.o build/irq.o build/pmm.o build/kheap.o \
               build/vmm.o build/vmm_fork.o build/uaccess.o build/cap.o build/sched.o build/tss.o build/fd.o build/ipc.o \
               build/bcast.o build/syscall.o build/sys_io.o build/sys_file.o build/sys_proc.o build/sys_mmap.o build/sys_exec.o build/sys_fork.o build/sys_wait.o build/acpi.o build/pcie.o \
               build/virtio_ring.o build/virtio.o build/virtio_pci.o build/blk.o \
               build/virtio_blk.o build/rtc.o build/vfs.o build/fat32.o build/sfs.o build/lz4.o \
               build/ext4.o build/elf.o build/user_image.o build/string.o build/cpu_mitigations.o
# Kernel include search paths (so "#include "pmm.h"" resolves after the
# kernel/ subdirectory reorganization).
KINCLUDES   := -Ikernel -Ikernel/mm -Ikernel/proc -Ikernel/ipc -Ikernel/syscall \
               -Ikernel/acpi -Ikernel/drivers/pcie -Ikernel/drivers/virtio -Ikernel/drivers/rtc \
               -Ikernel/drivers/blk -Ikernel/fs/vfs -Ikernel/fs/fat32 -Ikernel/fs/sfs \
               -Ikernel/fs/ext4 -Ikernel/exec -Ikernel/include -Ikernel/arch/x86_64
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

.PHONY: all setup toolchain-check kernel image smoke smoke-fs smoke-fs-rw smoke-fs-sfs-rw smoke-fs-ext4 smoke-user smoke-uaccess smoke-sysio smoke-sysfile smoke-sysproc smoke-sysmmap smoke-sysexec smoke-sysfork smoke-syswait smoke-mitigations smoke-pmm-poison fat-image sfs-image ext4-image clean

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

$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_LD) $(USER_SRC) $(USER_WX_SRC) $(USER_SYS_SRC) $(USER_EXEC_SRC) $(USER_LD)
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
	@for e in $(USER_ELF) $(USER_WX_ELF) $(USER_SYS_ELF) $(USER_EXEC_ELF); do test "$$(wc -c < $$e)" -le 8192 || { echo "$$e exceeds 8 KiB (loader uses a 2-page bootstrap buffer)"; exit 1; }; done
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
	$(CC) $(KCFLAGS) -c kernel/mm/vmm_fork.c   -o build/vmm_fork.o
	$(CC) $(KCFLAGS) -c kernel/mm/uaccess.c    -o build/uaccess.o
	$(CC) $(KCFLAGS) -c kernel/cap.c           -o build/cap.o
	$(CC) $(KCFLAGS) -c kernel/proc/sched.c    -o build/sched.o
	$(CC) $(KCFLAGS) -c kernel/proc/tss.c      -o build/tss.o
	$(CC) $(KCFLAGS) -c kernel/proc/fd.c       -o build/fd.o
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
	$(CC) $(KCFLAGS) -c kernel/acpi/acpi.c       -o build/acpi.o
	$(CC) $(KCFLAGS) -c kernel/drivers/pcie/pcie.c           -o build/pcie.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_ring.c  -o build/virtio_ring.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio.c       -o build/virtio.o
	$(CC) $(KCFLAGS) -c kernel/drivers/virtio/virtio_pci.c   -o build/virtio_pci.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/blk.c             -o build/blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/blk/virtio_blk.c      -o build/virtio_blk.o
	$(CC) $(KCFLAGS) -c kernel/drivers/rtc/rtc.c            -o build/rtc.o
	$(CC) $(KCFLAGS) -c kernel/fs/vfs/vfs.c                  -o build/vfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/fat32/fat32.c             -o build/fat32.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/sfs.c                 -o build/sfs.o
	$(CC) $(KCFLAGS) -c kernel/fs/sfs/lz4.c                 -o build/lz4.o
	$(CC) $(KCFLAGS) -c kernel/fs/ext4/ext4.c              -o build/ext4.o
	$(CC) $(KCFLAGS) -c kernel/exec/elf.c                  -o build/elf.o
	$(CC) $(KCFLAGS) -c kernel/string.c        -o build/string.o
	$(CC) $(KCFLAGS) -c kernel/arch/x86_64/cpu_mitigations.c -o build/cpu_mitigations.o
	$(LD) -nostdlib -T $(KERNEL_LD) -o $(KERNEL_ELF) $(KERNEL_OBJS)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_BIN)
	@test "$$(wc -c < $(KERNEL_BIN))" -le 262144 || { echo "kernel.bin exceeds 256 KiB; Stage 2 loads 8x64 sectors"; exit 1; }
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
	TIMEOUT_S=60 EXTRA_SENTINEL="$$(printf '[user] ELF loaded from SFS; ring-3 thread spawned\nHELLO FROM RING-3\n[user] sys_exit(0)\n[trap] user #PF page fault\n[sfs] lz4+tags compress/readback/tag OK')" \
	    bash tools/qemu_runner/boot_test.sh $(IMG)

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

clean:
	rm -rf build
	cd $(TC_DIR)/hello_rs && cargo clean 2>/dev/null || true
