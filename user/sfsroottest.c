/* user/sfsroottest.c — persistent SFS-root probe (DDR-760).
 *
 * Spawned with root_mnt = a freshly reformatted + provisioned SFS volume (the
 * kernel writes /etc/aether/config there after the destructive SFS self-tests).
 * Opens /etc/aether/config through its SFS root and verifies the content — proof
 * that a process can durably root at a clean SFS volume and read a real config
 * path. All-pass -> PRADYOS_SFSROOT_OK, else "SFSROOT FAIL" + exit 1.
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
    if (fd < 0) fail("open /etc/aether/config on SFS root");
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
