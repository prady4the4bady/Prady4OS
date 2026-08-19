#!/usr/bin/env python3
"""BUG-1 diagnostic target: smoke-fs-liveness.

Modelled byte-for-byte on smoke-rqstress-liveness (Makefile ~1958), which is the
in-tree pattern for running ONE gate against a BSP_LIVENESS=1 kernel and then
restoring the untraced image. The only differences are the gate's sentinels and
timeout, taken verbatim from the smoke-fs recipe at Makefile:748.

R2: no backslash escapes are written literally here. The sentinel string is
reproduced from the existing recipe using chr(92) for every backslash so the
Windows->WSL->heredoc->python chain cannot collapse them into real control
characters (which previously left a C string literal unterminated).
"""
import io
BS = chr(92)
TAB = chr(9)
p = "Makefile"
s = io.open(p, encoding="utf-8", newline="").read()

anchor = "# ---------------------------------------------------------------------------\n# ADR-034 - aarch64"
if anchor not in s:
    anchor = s[s.index("# ADR-034"):s.index("# ADR-034") + 20]
    idx = s.index(anchor)
    # back up to the start of the comment banner line
    idx = s.rindex("# ------", 0, idx)
else:
    idx = s.index(anchor)

sentinels = ("msix vec=56" + BS + "n"
             "PRADYOS filesystem works!" + BS + "n"
             "nested file ok" + BS + "n"
             "long name read works" + BS + "n"
             "[rtc] 20" + BS + "n"
             "kernel wrote this" + BS + "n"
             "created+deleted /TMP.TXT OK" + BS + "n"
             "create/lookup OK" + BS + "n"
             "byte-exact OK" + BS + "n"
             "journal abort/commit/replay OK" + BS + "n"
             "version-isolation OK" + BS + "n"
             "compress/readback/tag OK" + BS + "n"
             "[sfs] freelist persist OK" + BS + "n"
             "[pdrive] workspace OK")

block = (
"# DDR-777/790/791 (BUG-1): smoke-fs against a BSP_LIVENESS=1 kernel.\n"
"#\n"
"# smoke-fs is the gate that actually caught the wedge (CI run 32259190462):\n"
"# neither '[sfs] btree churn OK' nor '[sfs] churn FAIL' printed, so the BSP\n"
"# stopped INSIDE the 40-iteration DDR-763 churn loop -- it neither finished nor\n"
"# reported a failure. The downstream [pdrive] and freelist sentinels were simply\n"
"# never reached, which is why the gate blamed the freelist.\n"
"#\n"
"# smoke-rqstress-liveness already does this for the rqstress gate; this is the\n"
"# same harness pointed at the gate that reproduces. Same three-way read (DDR-777):\n"
"#   no [bsp] lines            -> BSP never reached churn; stall is EARLIER.\n"
"#   [bsp] stops at iter=N,\n"
"#     [hb] still ticking      -> wedged INSIDE iteration N, timer alive.\n"
"#   [bsp] reaches iter=39 and\n"
"#     'btree churn OK' prints -> churn completed; stall is AFTER it.\n"
"#\n"
"# Own target, NOT added to the shard matrix: BSP_LIVENESS is COMPILE-TIME and CI\n"
"# builds one shared image, so enabling it globally would put per-iteration output\n"
"# into every later gate -- DDR-790 proved that evicts smoke-dmesg's marker from\n"
"# the last-4 KiB log ring. Diagnostic only; remove once BUG-1 is closed.\n"
"smoke-fs-liveness: fat-image sfs-image\n"
+ TAB + "@echo \"[bsp-liveness] rebuilding kernel with BSP_LIVENESS=1 (DDR-777)\"\n"
+ TAB + "rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG)\n"
+ TAB + "$(MAKE) image BSP_LIVENESS=1\n"
+ TAB + "TIMEOUT_S=120 EXTRA_SENTINEL=\"$$(printf '" + sentinels + "')\" " + BS + "\n"
+ TAB + "    bash tools/qemu_runner/boot_test.sh $(IMG) ; " + BS + "\n"
+ TAB + "  rc=$$? ; " + BS + "\n"
+ TAB + "  echo \"[bsp-liveness] last churn iteration reached:\" ; " + BS + "\n"
+ TAB + "  echo \"[bsp-liveness] restoring untraced kernel\" ; " + BS + "\n"
+ TAB + "  rm -f $(BUILD_DIR)/main.o $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.bin $(IMG) ; " + BS + "\n"
+ TAB + "  $(MAKE) image ; " + BS + "\n"
+ TAB + "  exit $$rc\n"
"\n")

s = s[:idx] + block + s[idx:]

# .PHONY registration, next to the existing liveness target.
old_phony = "smoke-rqstress-liveness"
assert old_phony in s, "phony anchor"
s = s.replace(".PHONY: all setup", ".PHONY: smoke-fs-liveness all setup", 1)

io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("smoke-fs-liveness added")
