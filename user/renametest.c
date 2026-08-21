/* user/renametest.c — SYS_RENAME probe (DDR-956).
 *
 * Three arms, each chosen so that a STUB returning 0 fails at least one of them
 * (R8). A rename that "works" but leaves the source readable, or that reports
 * success for a path that never existed, is exactly the shape a naive
 * implementation takes — so success alone is not asserted.
 *
 *   arm 1  PRADYOS_RENAME_OK      rename(src -> dst) returns 0 and dst opens
 *   arm 2  PRADYOS_RENAME_OLD_GONE  src no longer opens after the rename
 *   arm 3  PRADYOS_RENAME_ENOENT  renaming an absent path FAILS (must not be 0)
 *
 * Arm 3 is the one that catches a stub: a handler that returns 0 unconditionally
 * passes arm 1 trivially and can pass arm 2 by accident, but cannot pass arm 3.
 *
 * The cross-mount (-EXDEV) arm from the original plan is deliberately absent:
 * every syscall passes t->root_mnt and no path->mount resolution exists in this
 * kernel, so a second mount cannot be named from ring 3. Printing an EXDEV
 * sentinel here would assert nothing about the kernel. See DDR-956 sec.2a.
 *
 * Freestanding (no libc): raw syscalls, NO writable globals (user.ld / DDR-826).
 */

#define SYS_WRITE   6
#define SYS_OPEN    7
#define SYS_CLOSE   8
#define SYS_EXIT    4
#define SYS_RENAME  95

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_CREAT     0x40

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("RENAME FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char *src = "/RENSRC.TXT";
    const char *dst = "/RENDST.TXT";

    /* ---- setup: create the source ------------------------------------- */
    long fd = nsi(SYS_OPEN, (long)src, O_WRONLY | O_CREAT, 0);
    if (fd < 0)
        fail("could not create source");
    nsi(SYS_CLOSE, fd, 0, 0);

    /* ---- arm 1: rename succeeds and the destination is readable -------- */
    long r = nsi(SYS_RENAME, (long)src, (long)dst, 0);
    if (r != 0)
        fail("rename of an existing file did not return 0");
    fd = nsi(SYS_OPEN, (long)dst, O_RDONLY, 0);
    if (fd < 0)
        fail("destination not readable after rename");
    nsi(SYS_CLOSE, fd, 0, 0);
    wr("PRADYOS_RENAME_OK\n");

    /* ---- arm 2: the source is gone ------------------------------------ */
    /* Discriminating: a copy-style implementation leaves the source in place
     * and would reach arm 1 while failing here. */
    fd = nsi(SYS_OPEN, (long)src, O_RDONLY, 0);
    if (fd >= 0) {
        nsi(SYS_CLOSE, fd, 0, 0);
        fail("source still readable after rename");
    }
    wr("PRADYOS_RENAME_OLD_GONE\n");

    /* ---- arm 3: renaming an absent path must FAIL ---------------------- */
    /* This is the stub-catcher. It also covers the missing-parent case, since
     * sfs_walk fails on an absent intermediate exactly as it does on an absent
     * leaf, and sys_rename collapses the backend's generic -1 to -ENOENT. */
    r = nsi(SYS_RENAME, (long)"/NOSUCH/DEEP.TXT", (long)"/RENX.TXT", 0);
    if (r == 0)
        fail("rename of an absent path returned success");
    wr("PRADYOS_RENAME_ENOENT\n");

    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
