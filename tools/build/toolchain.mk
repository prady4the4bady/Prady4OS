# tools/build/toolchain.mk
# Single source of truth for the PRADYOS cross toolchain (ADR-001).
# Included by the top-level Makefile and all phase makefiles.

# --- LLVM/Clang (C + assembly + linking) ------------------------------------
CC       := clang
LD       := ld.lld
OBJCOPY  := llvm-objcopy
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

# --- Rust (bare-metal userspace + safe kernel subsystems) -------------------
CARGO       := cargo +nightly
RUST_TARGET := x86_64-unknown-none
