# DDR-959 — `ci-probe-rodata-check` reported OK unconditionally

Status: ACCEPTED — defect found and fixed, two-arm verified.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## 1. What was wrong

`tools/ci/probe_rodata_check.sh` (DDR-826) exists to fail the build when a
freestanding probe ELF carries a writable allocated section. `user/user.ld`
links probes as one R+X `PT_LOAD`, so such a section has nowhere legal to go:
lld places it as an orphan inside the read-only segment, the link succeeds, and
the first store faults at runtime — which every gate then reports as a missing
sentinel, i.e. as a logic bug in the code under test. That misattribution is
what DDR-826 was written to stop.

**The check has never been able to report anything.** Two independent defects in
one line:

```sh
awk '/ WA / { if (strtonum("0x" $6) > 0) print $2 " (" strtonum("0x" $6) " bytes)" }'
```

1. **`strtonum()` is a GNU-awk extension.** On mawk — the default `/usr/bin/awk`
   on Ubuntu, so on the dev host *and* on `ubuntu-latest` runners — it is
   undefined and awk aborts the program with `function strtonum never defined`
   on the first matching line. `bad` is then always empty and the script prints
   its OK line whatever the ELFs contain.
2. **The field numbers were off by one for every section numbered 0–9.**
   `readelf -SW` prints those as `[ 3]`, with an inner space that survives
   `tr -s ' '` and splits into two fields. So `$2` was the index remnant `3]`
   and `$6` was the file OFFSET, not the size. Only sections numbered 10+ lined
   up, and probe ELFs have four sections — so none of them ever did.

Either defect alone makes the check vacuous. It has been reporting
"OK — N ELFs, none carry a writable allocated section" as a fixed string.

## 2. Fix

Strip the bracketed index with `sed` before splitting, so fields start at Name
unconditionally; parse the hex size with a portable `hex()` function instead of
`strtonum`; and match the flags column as a set (`$7 ~ /W/ && $7 ~ /A/`) rather
than the literal `" WA "`, which additionally catches `WAT` (`.tdata`).

## 3. Two-arm verification

Both arms measured on this host (mawk 1.3.4):

**Arm A — the tree is clean.** `make ci-probe-rodata-check` →
`OK — 56 ELFs, none carry a writable allocated section`. No false positive: the
musl-linked programs are skipped earlier by the writable-`PT_LOAD` test, and the
freestanding probes genuinely have no writable allocated section.

**Arm B — it can fail.** A two-line probe with one mutable global
(`static long counter = 1;`) linked against `user/user.ld` and dropped in
`build/`:

```
probe-rodata-check: FAIL — zzbadprobe.elf has writable allocated
                    section(s) but NO writable PT_LOAD to hold them:
  .data (8 bytes)
```

Run against the same ELF, the **old** parser prints
`awk: line 2: function strtonum never defined` twice and reports nothing — it
would have passed the bad probe. That contrast is the evidence that the fix is
what changed the verdict, not the test input.

Removing the injected ELF returns the check to OK.

## 4. Why this is recorded rather than just fixed

CLAUDE.md §0.3 already carries one instance of this exact failure mode — a guard
whose observations were all vacuous, to which two retracted root causes trace.
This is a second instance, in a check that runs on every build and is named in
the §7 hygiene list. Every "probe-rodata-check: OK" recorded in this repo's
history before this commit carries no information, and any conclusion that
leaned on one should be re-derived.

Found while validating DDR-958, which relied on this check.
