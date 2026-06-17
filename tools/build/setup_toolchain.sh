#!/usr/bin/env bash
# PRADYOS toolchain bootstrap — WSL2 / Ubuntu. Idempotent.
# Installs: LLVM/Clang + lld, NASM, QEMU, UEFI/image tooling, GDB, and the
# Rust nightly bare-metal target. See ADR-001.
set -euo pipefail

echo "[pradyos] installing apt packages (requires sudo)..."
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    clang lld llvm \
    nasm \
    make \
    qemu-system-x86 \
    ovmf mtools xorriso dosfstools \
    gdb-multiarch \
    curl ca-certificates

echo "[pradyos] installing Rust (rustup nightly + bare-metal target)..."
if ! command -v rustup >/dev/null 2>&1; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --default-toolchain nightly --profile minimal
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
fi
rustup toolchain install nightly --profile minimal
rustup component add rust-src --toolchain nightly
rustup target add x86_64-unknown-none --toolchain nightly

echo
echo "[pradyos] installed versions:"
clang --version | head -n1
ld.lld --version | head -n1
nasm -v
qemu-system-x86_64 --version | head -n1
rustc +nightly --version
cargo +nightly --version

echo
echo "[pradyos] toolchain bootstrap complete. Next: make toolchain-check"
