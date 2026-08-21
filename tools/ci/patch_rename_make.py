#!/usr/bin/env python3
"""DDR-956 step 2g: build renametest.elf, add smoke-rename, register shard."""
import io
TAB = chr(9); BS = chr(92)
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "Makefile"; s = rd(p)
assert "renametest" not in s, "already wired"

# 1) source/elf vars, beside the sfsroot probe
s = sub(s, "USER_SFSROOT_SRC := user/sfsroottest.c",
        "USER_RENAME_SRC  := user/renametest.c     # fs: SYS_RENAME probe (DDR-956)\n"
        "USER_RENAME_ELF  := build/renametest.elf\n"
        "USER_SFSROOT_SRC := user/sfsroottest.c", "vars")

# 2) compile+link rule, beside the sfsroot one
s = sub(s, TAB + "$(CC) $(USER_C_CFLAGS) -c $(USER_SFSROOT_SRC) -o build/sfsroottest.o",
        TAB + "$(CC) $(USER_C_CFLAGS) -c $(USER_RENAME_SRC) -o build/renametest.o\n"
        + TAB + "$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_RENAME_ELF) build/renametest.o\n"
        + TAB + "$(CC) $(USER_C_CFLAGS) -c $(USER_SFSROOT_SRC) -o build/sfsroottest.o", "rule")

# 3) size-budget list
s = sub(s, "$(USER_SFSROOT_ELF) $(USER_BIGWRITE_ELF)",
        "$(USER_SFSROOT_ELF) $(USER_RENAME_ELF) $(USER_BIGWRITE_ELF)", "budget")

# 4) gate target, next to smoke-blk-timeout
anchor = "smoke-blk-timeout: $(IMG) fat-image sfs-image\n"
assert s.count(anchor) == 1, "blk-timeout anchor"
extra = ("PRADYOS_RENAME_OK" + BS + "n"
         "PRADYOS_RENAME_OLD_GONE" + BS + "n"
         "PRADYOS_RENAME_ENOENT")
forbid = ("RENAME FAIL" + BS + "n"
          "[vblk] compl wait timeout" + BS + "n"
          "[vblk] slot wait timeout")
gate = ("# DDR-956: SYS_RENAME. The OLD_GONE and ENOENT arms are the discriminating\n"
        "# ones -- a copy-style rename passes OK but leaves the source readable, and a\n"
        "# stub returning 0 passes OK yet cannot report failure for an absent path.\n"
        "# 'RENAME FAIL' is forbidden so a probe that reaches its own error path fails\n"
        "# the gate rather than merely omitting a sentinel.\n"
        "smoke-rename: $(IMG) fat-image sfs-image\n"
        + TAB + "TIMEOUT_S=60 QEMU_PROBES=renametest " + BS + "\n"
        + TAB + "    EXTRA_SENTINEL=\"$$(printf '" + extra + "')\" " + BS + "\n"
        + TAB + "    FORBIDDEN_SENTINEL=\"$$(printf '" + forbid + "')\" " + BS + "\n"
        + TAB + "    bash tools/qemu_runner/boot_test.sh $(IMG)\n\n")
s = s[:s.index(anchor)] + gate + s[s.index(anchor):]
s = sub(s, ".PHONY: smoke-blk-timeout", ".PHONY: smoke-rename smoke-blk-timeout", "phony")
wr(p, s); print("Makefile wired")

p = "tools/ci/gate_shards.txt"; s = rd(p)
assert "smoke-rename" not in s, "shard already registered"
if not s.endswith(chr(10)): s += chr(10)
s += "4" + TAB + "smoke-rename" + TAB + "60" + chr(10)
wr(p, s); print("shard 4 registered")
