#!/usr/bin/env bash
# mk_notices.sh <out> — generate THIRD_PARTY_NOTICES.txt for the release ISO.
#
# Operator decision (PR #17, comment 5822830053, item 37): the ISO ships the
# notices lwIP (BSD-3-Clause) and musl (MIT, with BSD-licensed portions)
# require in a binary redistribution.
#
# GENERATED FROM THE VENDORED SOURCES AT BUILD TIME, never hand-copied: a
# committed copy would drift the day a submodule is bumped, and nothing would
# notice. Every notice below is read out of third_party/ as it is on disk.
# The script refuses (non-zero) if any source file it needs is missing, so a
# moved or renamed file breaks the ISO build instead of shipping a notice
# file with a hole in it.
set -euo pipefail

out="${1:?usage: mk_notices.sh <out>}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
tp="$root/third_party"

need() { [ -f "$1" ] || { echo "mk_notices: missing $1" >&2; exit 1; }; }

lwip_copying="$tp/lwip/COPYING"
lwip_init="$tp/lwip/src/include/lwip/init.h"
musl_copyright="$tp/musl/COPYRIGHT"
musl_version="$tp/musl/VERSION"
tre_h="$tp/musl/src/regex/tre.h"
des_c="$tp/musl/src/crypt/crypt_des.c"
sun_c="$tp/musl/src/math/__cos.c"
for f in "$lwip_copying" "$lwip_init" "$musl_copyright" "$musl_version" \
         "$tre_h" "$des_c" "$sun_c"; do need "$f"; done

ver() { sed -n "s/^#define LWIP_VERSION_$1 *\([0-9]*\).*/\1/p" "$lwip_init"; }
lwip_ver="$(ver MAJOR).$(ver MINOR).$(ver REVISION)"
musl_ver="$(tr -d '[:space:]' < "$musl_version")"

# The first C comment block of a file, with the comment markers stripped.
first_comment() {
    awk 'BEGIN{p=0} /\/\*/{p=1} p{print} p&&/\*\//{exit}' "$1" \
        | sed -e 's#^/\*##' -e 's#\*/$##' -e 's#^ \* \{0,1\}##' -e 's#^ \*$##'
}
# The FreeSec licence block inside crypt_des.c is the THIRD comment block.
nth_comment() {
    awk -v n="$2" 'BEGIN{c=0;p=0}
        /\/\*/{c++; if(c==n)p=1}
        p{print}
        p&&/\*\//{exit}' "$1" \
        | sed -e 's#^/\*##' -e 's#\*/$##' -e 's#^ \* \{0,1\}##' -e 's#^ \*$##'
}

rule() { printf '%s\n' "========================================================================"; }

{
    echo "PradyOS — THIRD-PARTY SOFTWARE NOTICES"
    echo
    echo "PradyOS itself is proprietary software; see LICENSE.txt on this medium."
    echo "Nothing in this file grants any right to PradyOS."
    echo
    echo "This distribution includes the following third-party components, each"
    echo "under its own licence reproduced below as that licence requires:"
    echo
    echo "  1. lwIP ${lwip_ver}        — TCP/IP stack, linked into the kernel."
    echo "     BSD-3-Clause."
    echo "  2. musl libc ${musl_ver}   — C library, statically linked into user programs."
    echo "     MIT, with portions under BSD and other permissive terms."
    echo
    rule
    echo "1. lwIP ${lwip_ver}"
    rule
    echo
    cat "$lwip_copying"
    echo
    rule
    echo "2. musl libc ${musl_ver} — COPYRIGHT file, reproduced in full"
    rule
    echo
    cat "$musl_copyright"
    echo
    rule
    echo "2a. musl: TRE regular-expression library (src/regex) — 2-clause BSD"
    rule
    first_comment "$tre_h"
    echo
    rule
    echo "2b. musl: FreeSec DES crypt (src/crypt/crypt_des.c) — BSD"
    rule
    nth_comment "$des_c" 3
    echo
    rule
    echo "2c. musl: Sun Microsystems FDLIBM-derived math code (src/math)"
    rule
    nth_comment "$sun_c" 2   # block 1 is the one-line "origin:" comment
    echo
    rule
    echo "Courtesy attribution (no licence obligation)"
    rule
    echo
    echo "The desktop's glyph atlas is rendered bitmaps rasterised from the Inter"
    echo "typeface, (c) The Inter Project Authors, SIL Open Font License 1.1"
    echo "(https://github.com/rsms/inter). No font software is distributed."
} > "$out"

# Self-check: the file must actually contain the operative licence sentences.
# Each pattern is chosen to occur ONLY in the section it proves: an earlier
# draft checked 'Sun Microsystems', which the musl COPYRIGHT summary also
# contains, so it passed while section 2c held nothing but an 'origin:' line.
# A generator that silently emitted headings with empty bodies (a moved file,
# a broken awk) would otherwise ship a notice file that satisfies nothing.
for pat in \
    'Swedish Institute of Computer Science' \
    'Redistributions in binary form must reproduce' \
    'Permission is hereby granted, free of charge' \
    'tre-internal.h - TRE internal definitions' \
    'FreeSec: libcrypt for NetBSD' \
    'software is freely granted, provided that this notice'; do
    grep -qF "$pat" "$out" || { echo "mk_notices: '$pat' missing from $out" >&2; exit 1; }
done
echo "notices: $out ($(wc -c < "$out") bytes; lwIP ${lwip_ver}, musl ${musl_ver})"
