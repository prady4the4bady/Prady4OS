#!/usr/bin/env bash
# tools/build_lwip.sh — build the lwIP 2.2.1 core into build/lwip/liblwip.a (NET-B).
#
# lwIP is third-party: compiled with the kernel toolchain but -w (not -Werror),
# per ADR-025 §D9. The PRADYOS port layer (third_party/lwip-port/) supplies
# lwipopts.h + arch/cc.h + arch/sys_arch.h; its .c shims (lwip_port.c, netif)
# are first-party and compiled by the kernel build, not here.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LWIP="$ROOT/third_party/lwip"
PORT="$ROOT/third_party/lwip-port"
OUT="$ROOT/build/lwip"
OBJ="$OUT/obj"

rm -rf "$OUT"
mkdir -p "$OBJ" "$OUT"

CC="clang --target=x86_64-elf"
# Mirror KCFLAGS (freestanding kernel, no SSE/red-zone), but -w for third-party.
# -nostdlibinc: keep clang's freestanding headers (stdint/stddef/limits/stdarg)
# but exclude the host glibc /usr/include (lwIP otherwise pulls a broken <stdio.h>).
CFLAGS="-ffreestanding -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone \
  -mgeneral-regs-only -fno-stack-protector -nostdlib -nostdlibinc -w -Os"
# Port headers first (lwipopts.h, arch/cc.h), then lwIP's own includes, then the
# kernel include dir so lwIP's <string.h> resolves to kernel/string.h.
INC="-I$PORT -I$LWIP/src/include -I$ROOT/kernel"

# Core + IPv4 + Ethernet netif. IPv6 and the non-ethernet netifs are excluded
# (LWIP_IPV6=0). Disabled features (dhcp/autoip/igmp/dns/altcp) compile to stubs.
SRCS=()
for f in "$LWIP"/src/core/*.c "$LWIP"/src/core/ipv4/*.c "$LWIP"/src/netif/ethernet.c; do
  SRCS+=("$f")
done

echo "[lwip] compiling ${#SRCS[@]} sources -> $OUT/liblwip.a"
OBJS=()
for s in "${SRCS[@]}"; do
  o="$OBJ/$(basename "${s%.c}").o"
  $CC $CFLAGS $INC -c "$s" -o "$o"
  OBJS+=("$o")
done

llvm-ar rcs "$OUT/liblwip.a" "${OBJS[@]}"
echo "[lwip] OK: $OUT/liblwip.a ($(wc -c < "$OUT/liblwip.a") bytes, ${#OBJS[@]} objs)"
