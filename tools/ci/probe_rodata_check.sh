#!/usr/bin/env bash
# tools/ci/probe_rodata_check.sh — DDR-826. No probe ELF may contain a writable
# allocated section.
#
# `user/user.ld` links each probe as a SINGLE R+X PT_LOAD. There is no writable
# segment by design (W^X by construction, ADR-021). So any writable allocated
# section — `.data`, `.bss`, or their large-code-model twins `.ldata`/`.lbss` —
# has nowhere legal to go. lld does not error: it places the orphan inside the
# read-only segment, the link succeeds, and the FIRST STORE at runtime faults:
#
#   [trap] user #PF pid=29 rip=0x80000035D4 cr2=0x8000004F80 err=0x7
#            err bit1 = WRITE, bit2 = USER  ->  user write to a present RO page
#
# That is precisely how smoke-ed25519 failed. `ed25519.c` cached its derived
# curve constants in `static fe C_D` etc; those landed in a 264-byte `.lbss`;
# the probe spawned, `curve_init()` stored to it, and the process was killed
# before printing a single byte. The gate reported "sentinel not found", which
# reads as "the crypto is wrong" and is nothing of the kind.
#
# The failure is silent at build time and expensive at runtime, which is the
# same shape as the five instances before it (DDR-817/822/823/824/825). So it is
# asserted here rather than remembered.
#
# Host-only, instant. Runs on every build.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
rc=0

for elf in "$ROOT"/build/*.elf; do
    [ -f "$elf" ] || continue

    # Does this ELF HAVE a writable PT_LOAD? Ask the binary rather than
    # maintaining a list of which programs use which linker script — such a list
    # would be the seventh hand-maintained thing in this repo to drift silently.
    #
    # The musl-linked programs (init, prism, compositor, aether_daemon, ...) link
    # with $(USER_C_LD), which DOES provide a writable segment, so writable data
    # is correct for them. The freestanding probes link with user/user.ld, which
    # deliberately provides only R+X. Only the latter are the hazard.
    if readelf -lW "$elf" 2>/dev/null | grep -qE '^  LOAD .*(RW|WE)'; then
        continue
    fi

    # Parsing note — this awk was rewritten because the previous one reported
    # nothing on ANY input, i.e. this whole check was passing vacuously:
    #
    #   1. `strtonum()` is a GNU-awk extension. On mawk (the default /usr/bin/awk
    #      on Ubuntu, so on this host AND on ubuntu-latest runners) it is
    #      undefined, and awk aborts the program with "function strtonum never
    #      defined" on the first matching line. The check then printed its OK
    #      line regardless of what the ELFs contained.
    #   2. The field numbers were off by one for every section numbered 0-9.
    #      readelf prints those as `[ 3]` with an inner space, which splits into
    #      two fields, so `$2` was the index remnant `3]` and `$6` was the file
    #      OFFSET, not the size. Only sections 10+ lined up. Probe ELFs have
    #      four sections, so none of them ever did.
    #
    # Fixed by stripping the bracketed index before splitting (fields then start
    # at Name unconditionally) and parsing the hex size in portable awk. Flags
    # are matched as a set rather than the literal " WA ", so `WAT` (.tdata) is
    # caught too.
    bad="$(readelf -SW "$elf" 2>/dev/null | sed -E 's/^ *\[ *[0-9]+\] *//' \
           | awk 'function hex(s,   i,c,v,n) {
                      n = 0; s = tolower(s);
                      for (i = 1; i <= length(s); i++) {
                          c = substr(s, i, 1);
                          v = index("0123456789abcdef", c);
                          if (v == 0) return -1;
                          n = n * 16 + (v - 1);
                      }
                      return n;
                  }
                  $7 ~ /W/ && $7 ~ /A/ { if (hex($5) > 0) print $1 " (" hex($5) " bytes)" }')"
    if [ -n "$bad" ]; then
        echo "probe-rodata-check: FAIL — $(basename "$elf") has writable allocated" >&2
        echo "                    section(s) but NO writable PT_LOAD to hold them:" >&2
        printf '  %s\n' "$bad" >&2
        rc=1
    fi
done

if [ $rc -ne 0 ]; then
    cat >&2 <<'MSG'
probe-rodata-check: user/user.ld links probes as one R+X PT_LOAD with NO writable
                    segment. A writable section is placed as an orphan inside it
                    and the first store faults at runtime with #PF err=0x7 —
                    which the gate reports as a missing sentinel, i.e. it looks
                    like a logic bug in the code under test.

                    Remove the mutable global. Derive the value into a
                    caller-provided struct on the stack instead; see the ed_ctx
                    pattern at the top of kernel/crypto/ed25519.c.
MSG
    exit 1
fi

n=$(ls "$ROOT"/build/*.elf 2>/dev/null | wc -l)
echo "probe-rodata-check: OK — $n ELFs, none carry a writable allocated section"
