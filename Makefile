# PRADYOS top-level Makefile — Phase 0 (toolchain + skeleton).
# Real kernel/boot targets arrive in later phases. For now this only proves
# the toolchain is sound and wires the QEMU smoke test.

include tools/build/toolchain.mk

BUILD_DIR := build/toolchain
TC_DIR    := tests/toolchain
RUST_LIB  := $(TC_DIR)/hello_rs/target/$(RUST_TARGET)/release/libhello_rs.a

# Phase 1 — PRADYOS-BOOT
BOOT_DIR  := boot/mbr
BOOT_BIN  := build/boot.bin
IMG       := build/pradyos.img

.PHONY: all setup toolchain-check image smoke clean

all:
	@echo "PRADYOS — Phase 1 (PRADYOS-BOOT, legacy BIOS MBR slice)."
	@echo "  make setup            # install the toolchain (WSL2/Ubuntu)"
	@echo "  make toolchain-check  # prove clang + nasm + rust + lld interoperate"
	@echo "  make image            # assemble the MBR -> $(IMG)"
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

# Assemble the 512-byte MBR and lay it down as sector 0 of a 1 MiB raw disk.
# Padding past 512 bytes only keeps BIOS/QEMU from treating the image as a
# degenerate 1-sector disk; the boot sector itself is still sector 0.
image: $(IMG)

$(IMG): $(BOOT_DIR)/boot.asm
	@mkdir -p build
	$(NASM) -f bin $(BOOT_DIR)/boot.asm -o $(BOOT_BIN)
	@test "$$(wc -c < $(BOOT_BIN))" -eq 512 || { echo "boot.bin is not 512 bytes"; exit 1; }
	cp $(BOOT_BIN) $(IMG)
	truncate -s 1M $(IMG)
	@echo "image: $(IMG) (boot sector $$(wc -c < $(BOOT_BIN)) bytes)"

smoke: $(IMG)
	bash tools/qemu_runner/boot_test.sh $(IMG)

clean:
	rm -rf build
	cd $(TC_DIR)/hello_rs && cargo clean 2>/dev/null || true
