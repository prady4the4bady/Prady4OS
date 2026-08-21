#!/usr/bin/env python3
"""Drop ONLY the embedded-probe blob: kernel.bin is at the 1 MiB stage-2 window.

Keeps sfs_rename, vfs_rename, SYS_RENAME and the PRISM mv builtin -- all of
which built clean at 4287d122. Removes the incbin and the spawn block, whose
5,592-byte blob pushed kernel.bin 382 bytes past the DDR-733/827 boot window.
user/renametest.c is kept on disk for whichever option is chosen next.
"""
import io, re
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)

p = "arch/x86_64/user_image.asm"; s = rd(p)
blk = ("; DDR-956: ring-3 SYS_RENAME probe\n"
       "global renametest_elf\n"
       "global renametest_elf_end\n"
       "renametest_elf:\n"
       '    incbin "build/renametest.elf"\n'
       "renametest_elf_end:\n\n")
assert blk in s, "incbin block"
wr(p, s.replace(blk, "", 1)); print("incbin removed")

p = "kernel/main.c"; s = rd(p)
ext = ("extern const unsigned char renametest_elf[];          /* DDR-956: SYS_RENAME probe */\n"
       "extern const unsigned char renametest_elf_end[];\n")
assert ext in s, "externs"
s = s.replace(ext, "", 1)
i = s.index('                /* DDR-956: SYS_RENAME probe.')
j = s.index('                if (probe_enabled("ftruncate")) {', i)
s = s[:i] + s[j:]
wr(p, s); print("spawn removed")

p = "Makefile"; s = rd(p)
s = s.replace("USER_RENAME_SRC  := user/renametest.c     # fs: SYS_RENAME probe (DDR-956)\nUSER_RENAME_ELF  := build/renametest.elf\n", "", 1)
s = s.replace(chr(9) + "$(CC) $(USER_C_CFLAGS) -c $(USER_RENAME_SRC) -o build/renametest.o\n"
              + chr(9) + "$(LD) -nostdlib --strip-all -T $(USER_LD) -o $(USER_RENAME_ELF) build/renametest.o\n", "", 1)
s = s.replace("$(USER_SFSROOT_ELF) $(USER_RENAME_ELF) $(USER_BIGWRITE_ELF)",
              "$(USER_SFSROOT_ELF) $(USER_BIGWRITE_ELF)", 1)
i = s.index("# DDR-956: SYS_RENAME. The OLD_GONE and ENOENT arms")
j = s.index("smoke-blk-timeout: $(IMG)", i)
s = s[:i] + s[j:]
s = s.replace(".PHONY: smoke-rename smoke-blk-timeout", ".PHONY: smoke-blk-timeout", 1)
wr(p, s); print("Makefile reverted")

p = "tools/ci/gate_shards.txt"; s = rd(p)
s = s.replace("4" + chr(9) + "smoke-rename" + chr(9) + "60" + chr(10), "", 1)
wr(p, s); print("shard entry removed")
