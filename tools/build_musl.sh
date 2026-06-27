#!/usr/bin/env bash
# tools/build_musl.sh — build the PROC-D minimal musl subset (ADR-023).
#
# Hand-compiles the musl 1.2.5 sources needed for printf + malloc + the string
# subset against the PRADYOS NSI, producing build/musl/lib/libc.a and crt1.o. We
# do NOT run musl's configure; instead we generate the two headers musl's build
# would generate (bits/alltypes.h, bits/syscall.h) and put our NSI syscall-number
# overrides on top. musl is third-party source built with musl's own warning
# posture (NOT our -Werror surface, per ADR-023); our overlay shim is tiny asm.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL="$ROOT/third_party/musl"
OVL="$ROOT/third_party/musl-overlay"
OUT="$ROOT/build/musl"
GENI="$OUT/include"            # generated headers (musl's obj/include equivalent)
OBJ="$OUT/obj"
LIB="$OUT/lib"

rm -rf "$OUT"
mkdir -p "$GENI/bits" "$OBJ" "$LIB"

# --- generated headers (mirror musl's Makefile rules) -----------------------
# The musl submodule may be checked out with CRLF on Windows; sed/awk choke on
# the \r, so strip it from the generator inputs (clang tolerates CRLF in C/asm).
nocr() { tr -d '\r' < "$1"; }

# bits/alltypes.h: from the arch + generic .in via mkalltypes.sed
sed -f <(nocr "$MUSL/tools/mkalltypes.sed") \
    <(nocr "$MUSL/arch/x86_64/bits/alltypes.h.in") \
    <(nocr "$MUSL/include/alltypes.h.in") > "$GENI/bits/alltypes.h"

# bits/syscall.h: the native __NR_ table, but every number offset by +4096 so it
# is >= MAX_SYSCALLS (64) and dispatches to -ENOSYS (no accidental mis-dispatch
# to an unrelated NSI handler), then the SYS_ aliases, then our NSI overrides for
# the calls we actually implement.
nocr "$MUSL/arch/x86_64/bits/syscall.h.in" | \
    awk '$1=="#define"{ print "#define", $2, ($3 + 4096) }' > "$GENI/bits/syscall.h"
nocr "$MUSL/arch/x86_64/bits/syscall.h.in" | \
    awk '$1=="#define"{ n=$2; sub(/__NR_/,"SYS_",n); print "#define", n, ($3 + 4096) }' \
    >> "$GENI/bits/syscall.h"
cat "$OVL/syscall_overrides.h" >> "$GENI/bits/syscall.h"

# version.h (some TUs include it)
echo '#define VERSION "1.2.5"' > "$GENI/version.h"

# --- toolchain ---------------------------------------------------------------
CC="clang --target=x86_64-elf"
# musl's own posture + our requirements: freestanding, no system headers, large
# code model (user base 0x8000000000 exceeds 32-bit relocs), no stack protector.
# -w: musl is third-party source built with musl's own (non--Werror) warning
# posture per ADR-023 — silence clang's defaults (e.g. -Wshift-op-parentheses,
# which musl's style trips) rather than pollute our build output.
CFLAGS="-std=c99 -nostdinc -ffreestanding -fno-pic -fno-pie -mcmodel=large \
  -fno-stack-protector -fexcess-precision=standard -frounding-math \
  -Wa,--noexecstack -D_XOPEN_SOURCE=700 -Os -pipe -fomit-frame-pointer -w"
INC="-I$GENI -I$MUSL/arch/x86_64 -I$MUSL/arch/generic \
  -I$MUSL/src/include -I$MUSL/src/internal -I$MUSL/include"

# --- source subset (printf + malloc + string + startup/TLS) ------------------
SRCS=(
  # startup + libc core
  src/env/__libc_start_main.c
  src/env/__init_tls.c
  src/env/__stack_chk_fail.c
  src/internal/libc.c
  src/internal/syscall_ret.c
  src/internal/defsysinfo.c
  src/internal/vdso.c
  src/errno/__errno_location.c
  # threads / locking (single-thread paths, but referenced)
  src/thread/__lock.c
  src/thread/__wait.c
  src/thread/default_attr.c
  src/thread/pthread_self.c
  # exit
  src/exit/exit.c
  src/exit/_Exit.c
  src/exit/atexit.c
  # stdio write path
  src/stdio/printf.c
  src/stdio/vprintf.c
  src/stdio/vfprintf.c
  src/stdio/fwrite.c
  src/stdio/fputs.c
  src/stdio/puts.c
  src/stdio/__towrite.c
  src/stdio/__overflow.c
  src/stdio/__stdio_write.c
  src/stdio/__stdout_write.c
  src/stdio/__stdio_close.c
  src/stdio/__stdio_seek.c
  src/stdio/__lockfile.c
  src/stdio/stdout.c
  src/stdio/ofl.c
  src/stdio/ofl_add.c
  src/stdio/fflush.c
  # malloc (mallocng)
  src/malloc/mallocng/malloc.c
  src/malloc/mallocng/free.c
  src/malloc/mallocng/realloc.c
  src/malloc/mallocng/aligned_alloc.c
  src/malloc/mallocng/malloc_usable_size.c
  src/malloc/mallocng/donate.c
  src/malloc/calloc.c
  src/malloc/libc_calloc.c
  # string subset
  src/string/memcpy.c
  src/string/memmove.c
  src/string/memset.c
  src/string/memchr.c
  src/string/strlen.c
  src/string/strnlen.c
  src/string/strchr.c
  src/string/strcmp.c
  src/string/strncmp.c
  src/string/strcpy.c
  src/string/stpcpy.c
)

echo "[musl] compiling ${#SRCS[@]} sources -> $LIB/libc.a"
OBJS=()
for s in "${SRCS[@]}"; do
  o="$OBJ/$(echo "$s" | tr '/' '_').o"
  $CC $CFLAGS $INC -c "$MUSL/$s" -o "$o"
  OBJS+=("$o")
done

# our overlay __set_thread_area (replaces musl's arch_prctl version)
$CC $CFLAGS $INC -c "$OVL/__set_thread_area.s" -o "$OBJ/ovl___set_thread_area.o"
OBJS+=("$OBJ/ovl___set_thread_area.o")

rm -f "$LIB/libc.a"
llvm-ar rcs "$LIB/libc.a" "${OBJS[@]}"

# crt1.o (startup; links into the program, not the archive)
$CC $CFLAGS $INC -c "$MUSL/crt/crt1.c" -o "$LIB/crt1.o"

echo "[musl] OK: $LIB/libc.a ($(wc -c < "$LIB/libc.a") bytes), $LIB/crt1.o"
