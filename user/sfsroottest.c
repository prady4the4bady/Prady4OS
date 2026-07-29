/* user/sfsroottest.c — persistent SFS-root probe (DDR-760).
 *
 * Spawned with root_mnt = a freshly reformatted + provisioned SFS volume (the
 * kernel writes /etc/aether/config there after the destructive SFS self-tests).
 * Opens /etc/aether/config through its SFS root and verifies the content — proof
 * that a process can durably root at a clean SFS volume and read a real config
 * path.
 *
 * Three-way verdict (DDR-799), because "config absent" is a precondition miss
 * rather than a defect — kmain provisions it onto this root only on one path:
 *   config present + correct -> PRADYOS_SFSROOT_OK
 *   config absent            -> SFSROOT SKIP (this gate rooted elsewhere)
 *   config present + wrong   -> SFSROOT FAIL
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_READ   5
#define SYS_WRITE  6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_EXIT   4

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
    wr("SFSROOT FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    /* Resolve /etc/aether/config against the SFS root (mkdir-p'd + written by the
     * kernel). This exercises a multi-level SFS path from ring 3 at runtime. */
    long fd = nsi(SYS_OPEN, (long)"/etc/aether/config", 0 /*O_RDONLY*/, 0);
    if (fd < 0) {
        /* DDR-799: absent config is a PRECONDITION miss, not a failure.
         *
         * kmain writes /etc/aether/config onto this probe's root only on the
         * path that provisions a clean SFS volume (main.c ~1327). Gates that
         * root the daemon elsewhere — smoke-aether-sfsroot sets QEMU_SFSROOT=1,
         * so the provisioned mkfs image is used instead — leave this probe's
         * root without a config, and it was reporting that as a failure in a
         * gate that never asked for it.
         *
         * The assertion is owned by smoke-sfsroot, which REQUIRES
         * PRADYOS_SFSROOT_OK; a genuine regression fails there on a missing
         * required sentinel, which cannot be masked. What survives as FAIL here
         * is the case that is unambiguously wrong: the config EXISTS but does
         * not read back correctly. */
        wr("SFSROOT SKIP: no provisioned config on this root\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    char buf[64];
    long n = nsi(SYS_READ, fd, (long)buf, (long)sizeof buf - 1);
    nsi(SYS_CLOSE, fd, 0, 0);
    if (n <= 0) fail("read returned no data");
    buf[n] = 0;

    /* The kernel provisioned it starting with "mode=sovereign". */
    const char *want = "mode=sovereign";
    for (int i = 0; want[i]; i++)
        if (buf[i] != want[i]) fail("config content mismatch");

    wr("PRADYOS_SFSROOT_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
