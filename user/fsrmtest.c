/* user/fsrmtest.c — ring-3 file-lifecycle probe (DDR-744).
 *
 * Spawned with its root_mnt set to the SFS volume (the writable CoW root).
 * Exercises the two new ring-3 VFS mutations end-to-end across the syscall
 * boundary:
 *   1. open("/RMPROBE", O_CREAT|O_WRONLY) -> create, write bytes, close.
 *   2. open("/RMPROBE", O_RDONLY)         -> the create landed; read back, close.
 *   3. unlink("/RMPROBE")                 -> 0.
 *   4. open("/RMPROBE", O_RDONLY)         -> < 0 (gone).
 *   5. unlink("/RMPROBE") again           -> < 0 (already gone; idempotent-safe).
 * All pass -> PRADYOS_FSRM_OK; any step wrong -> "FSRM FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE   6
#define SYS_READ    5
#define SYS_OPEN    7
#define SYS_CLOSE   8
#define SYS_EXIT    4
#define SYS_UNLINK 68

#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_CREAT   0x40

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
    wr("FSRM FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char *path = "/RMPROBE";
    const char *payload = "rm-probe payload";
    long plen = slen(payload);

    /* (1) create-on-open, write, close. */
    long fd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (fd < 0) fail("O_CREAT open failed");
    if (nsi(SYS_WRITE, fd, (long)payload, plen) != plen) fail("write short");
    nsi(SYS_CLOSE, fd, 0, 0);

    /* (2) the file now exists on the SFS root — read it back. */
    long rf = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (rf < 0) fail("created file did not persist");
    char buf[32];
    long n = nsi(SYS_READ, rf, (long)buf, plen);
    nsi(SYS_CLOSE, rf, 0, 0);
    if (n != plen) fail("readback short");
    for (long i = 0; i < plen; i++)
        if (buf[i] != payload[i]) fail("readback mismatch");

    /* (3) remove it. */
    if (nsi(SYS_UNLINK, (long)path, 0, 0) != 0) fail("unlink failed");

    /* (4) it must be gone. */
    long gf = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (gf >= 0) { nsi(SYS_CLOSE, gf, 0, 0); fail("file still open-able after unlink"); }

    /* (5) unlinking an absent name is a clean error, not a crash. */
    if (nsi(SYS_UNLINK, (long)path, 0, 0) >= 0) fail("second unlink succeeded");

    wr("PRADYOS_FSRM_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
