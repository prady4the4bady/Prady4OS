import io
p = "tools/ci/build_check.sh"
s = io.open(p, encoding="utf-8", newline="").read()

old_w = 'warns=$(grep -i "warning" "$LOG" | grep -cvE "^make(\[[0-9]+\])?: ")\n'
assert s.count(old_w) == 1, "warn line"
# Match "warning:" WITH the colon: clang/nasm diagnostics look like
# "file.c:12:3: warning: ...". Plain "warning" also matches Makefile COMMENT
# text echoed during the build ("# (no warnings from them) ..."), which is not
# a diagnostic. Also drop make's own "make: warning: Clock skew" lines, which
# come from Windows/WSL mtime drift and say nothing about the code.
new_w = 'warns=$(grep -i "warning:" "$LOG" | grep -cvE "^make(\[[0-9]+\])?: ")\n'
s = s.replace(old_w, new_w, 1)

old_h = """    echo "build_check: HASH UNCHANGED — your edit was NOT compiled into $BIN."
    echo "build_check: touch the changed source and re-run; if it still does not"
    echo "build_check: move, the Makefile dependency for that file is broken."
    hashmoved=0"""
assert s.count(old_h) == 1, "hash block"
# An unchanged hash is only a DEFECT when the content changed. Rebuilding
# identical sources deterministically yields an identical binary, which is
# correct. So report it and let the caller decide: --expect-change makes it
# fatal, which is what you want right after editing a file.
new_h = """    echo "build_check: HASH UNCHANGED."
    echo "build_check:   If you just EDITED a source, this means the edit was"
    echo "build_check:   NOT compiled in — investigate the Makefile dependency."
    echo "build_check:   If you only re-ran a build with no content change, this"
    echo "build_check:   is correct (the build is deterministic)."
    hashmoved=0"""
s = s.replace(old_h, new_h, 1)

old_x = '[ "$hashmoved" -eq 1 ] || exit 4\n'
assert s.count(old_x) == 1, "exit line"
new_x = ('if [ "${1:-}" = "--expect-change" ] && [ "$hashmoved" -ne 1 ]; then\n'
         '    echo "build_check: FAIL — --expect-change given but the binary did not move"\n'
         '    exit 4\n'
         'fi\n')
s = s.replace(old_x, new_x, 1)

io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("build_check refined")
