#!/usr/bin/env bash
# tools/build_musl_so.sh — build musl's shared libc.so (which is also its ld.so)
# against the PRADYOS NSI. DDR-1189 Amendment 5, P3b-1 and P3b-2.
#
# musl's own configure and Makefile do the work: its source selection, flags and
# link line (-shared, -e _dlstart, --dynamic-list), so no hand-written source
# list can drift from upstream. The submodule is never written. The build runs
# from a symlink copy of it (cp -rs) in which exactly two files are replaced:
#
#   arch/x86_64/bits/syscall.h.in  every __NR_ number is its NSI number if one of
#                                  the two override files names it, else Linux's
#                                  number + 4096 (>= MAX_SYSCALLS, so -ENOSYS).
#                                  musl's Makefile derives SYS_ names from these
#                                  lines with sed, so the file has no #undef.
#   src/thread/x86_64/__set_thread_area.s
#                                  the overlay's (SYS_SET_TLS), as libc.a uses.
#
# musl is third-party source built with musl's own warning posture (ADR-023).
# Output: build/musl-so/lib/libc.so, lib/Scrt1.o, lib/crti.o, lib/crtn.o, and
# the headers under build/musl-so/tree + build/musl-so/obj/include.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL="$ROOT/third_party/musl"
OVL="$ROOT/third_party/musl-overlay"
OUT="$ROOT/build/musl-so"
TREE="$OUT/tree"

[ -f "$MUSL/ldso/dynlink.c" ] || { echo "build_musl_so: $MUSL is not checked out" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$TREE"
cp -rs "$MUSL/." "$TREE/"

# --- the syscall numbers ------------------------------------------------------
SC="$TREE/arch/x86_64/bits/syscall.h.in"
rm -f "$SC"
python3 - "$MUSL/arch/x86_64/bits/syscall.h.in" "$OVL/syscall_overrides.h" \
          "$OVL/syscall_overrides_so.h" "$SC" <<'PY'
import re, sys
native, o1, o2, out = sys.argv[1:5]
ov = {}
for f in (o1, o2):
    for line in open(f, encoding='utf-8'):
        m = re.match(r'\s*#define\s+__NR_(\w+)\s+(\d+)\b', line)
        if m:
            ov[m.group(1)] = int(m.group(2))
seen, lines = set(), []
for line in open(native, encoding='utf-8'):
    line = line.replace('\r', '')
    m = re.match(r'#define\s+__NR_(\w+)\s+(\d+)\s*$', line)
    if not m:
        continue
    name, num = m.group(1), int(m.group(2))
    seen.add(name)
    lines.append('#define __NR_%s %d\n' % (name, ov.get(name, num + 4096)))
missing = sorted(set(ov) - seen)
if missing:
    sys.exit('build_musl_so: override names not in musl: %s' % ' '.join(missing))
open(out, 'w').writelines(lines)
print('build_musl_so: %d syscall numbers, %d mapped to NSI' % (len(lines), len(ov)))
PY

# --- the thread pointer -------------------------------------------------------
STA="$TREE/src/thread/x86_64/__set_thread_area.s"
rm -f "$STA"
cp "$OVL/__set_thread_area.s" "$STA"
# MUTANT D5: rewrite the copy of ldso/dynlink.c
DL="$TREE/ldso/dynlink.c"
python3 - "$MUSL/ldso/dynlink.c" "$DL" <<'PY'
import sys
s = open(sys.argv[1]).read()
old = '\t\tcase REL_GOT:\n'
assert s.count(old) == 1, old
import os
os.remove(sys.argv[2])
open(sys.argv[2], "w").write(s.replace(old, '\t\tcase REL_GOT:\n\t\t\tif (type == REL_GOT && dso != &ldso) break;\n'))
PY

# --- musl's own build -----------------------------------------------------------
mkdir -p "$OUT/b"
cd "$OUT/b"
"$TREE/configure" --target=x86_64 --disable-static --prefix=/ \
    CC="clang --target=x86_64-linux-gnu" > "$OUT/configure.log" 2>&1 || {
    echo "build_musl_so: configure failed; see $OUT/configure.log" >&2; exit 1; }
make -j"$(nproc)" lib/libc.so lib/Scrt1.o lib/crti.o lib/crtn.o \
    obj/include/bits/alltypes.h obj/include/bits/syscall.h > "$OUT/make.log" 2>&1 || {
    echo "build_musl_so: make failed; see $OUT/make.log" >&2; tail -20 "$OUT/make.log" >&2; exit 1; }
mkdir -p "$OUT/lib" "$OUT/obj"
cp lib/libc.so lib/Scrt1.o lib/crti.o lib/crtn.o "$OUT/lib/"
cp -r obj/include "$OUT/obj/"

# --- checks: the shim reached the binary --------------------------------------
grep -q '^#define SYS_open 7$' "$OUT/obj/include/bits/syscall.h" || {
    echo "build_musl_so: SYS_open is not NSI 7 in the generated header" >&2; exit 1; }
grep -q '^#define SYS_pread64 4113$' "$OUT/obj/include/bits/syscall.h" || {
    echo "build_musl_so: SYS_pread64 is not Linux+4096 (P3b-5)" >&2; exit 1; }
echo "build_musl_so: $(stat -c %s "$OUT/lib/libc.so") bytes build/musl-so/lib/libc.so"
