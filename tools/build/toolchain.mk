# tools/build/toolchain.mk
# Single source of truth for the PRADYOS cross toolchain (ADR-001).
# Included by the top-level Makefile and all phase makefiles.

# --- LLVM/Clang (C + assembly + linking) ------------------------------------
CC       := clang
LD       := ld.lld
OBJCOPY  := llvm-objcopy
NM       := nm
NASM     := nasm

# x86_64 freestanding triple. Pinned after `make toolchain-check` proves it
# links on this machine; fallbacks documented in ADR-001.
X64_TRIPLE := x86_64-elf

# General freestanding C flags for kernel/arch code (phase 1+).
# (toolchain-check uses its own minimal flags; see the Makefile.)
CFLAGS_X64 := --target=$(X64_TRIPLE) -ffreestanding -fno-stack-protector \
              -fno-stack-check -mno-red-zone -mcmodel=kernel -nostdlib \
              -Wall -Wextra

NASMFLAGS_X64 := -f elf64

# --- Additional architectures (ADR-034) -------------------------------------
# clang is the cross-compiler; no GCC cross-toolchain is installed. Assembly for
# these targets is GAS syntax through clang's integrated assembler — nasm stays
# x86-only.
ARM64_TRIPLE := aarch64-none-elf
RV64_TRIPLE  := riscv64-none-elf

# -mgeneral-regs-only on aarch64: no FP/SIMD in kernel context, matching the
# x86_64 build's stance. FP state is not saved across the boot path.
CFLAGS_ARM64 := --target=$(ARM64_TRIPLE) -ffreestanding -fno-pic -fno-pie \
                -mgeneral-regs-only -fno-stack-protector -nostdlib \
                -Wall -Wextra -Werror

# -march=rv64imac: no FP either, and -mabi=lp64 (not lp64d) so the ABI matches.
CFLAGS_RV64  := --target=$(RV64_TRIPLE) -ffreestanding -fno-pic -fno-pie \
                -march=rv64imac -mabi=lp64 -mcmodel=medany \
                -fno-stack-protector -nostdlib -Wall -Wextra -Werror

# --- Rust (bare-metal userspace + safe kernel subsystems) -------------------
CARGO       := cargo +nightly
RUST_TARGET := x86_64-unknown-none
