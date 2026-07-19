/* user/bigwritetest.c — ring-3 large-write probe (DDR-764).
 *
 * Spawned with root_mnt = the persistent SFS root. Writes 8 KiB to /BIG.TXT in
 * one SYS_WRITE, reads it back, and verifies the bytes. With the old 256-byte
 * FD_VFS chunk this short-writes at ~1 KiB (5th SFS extent rejected) -> readback
 * mismatch; with the 4 KiB chunk the 8 KiB lands in 2 extents. All-pass ->
 * PRADYOS_BIGWRITE_OK, else "BIGWRITE FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_READ   5
#define SYS_WRITE  6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_EXIT   4
#define O_WRONLY   0x1
#define O_CREAT    0x40

#define BIGLEN 8192

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
    wr("BIGWRITE FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    /* Stack buffers only — user.ld forbids writable globals (R+X segment). The
     * ring-3 stack is 8 MiB (ADR-021), so 2*8 KiB is fine. */
    char pat[BIGLEN];
    for (int i = 0; i < BIGLEN; i++)
        pat[i] = (char)(i * 7 + 3);

    long fd = nsi(SYS_OPEN, (long)"/BIG.TXT", O_CREAT | O_WRONLY, 0);
    if (fd < 0) fail("O_CREAT open");
    long w = nsi(SYS_WRITE, fd, (long)pat, BIGLEN);
    nsi(SYS_CLOSE, fd, 0, 0);
    if (w != BIGLEN) fail("short write (chunk/extent limit)");

    long rf = nsi(SYS_OPEN, (long)"/BIG.TXT", 0 /*O_RDONLY*/, 0);
    if (rf < 0) fail("reopen");
    char rbuf[BIGLEN];
    long n = nsi(SYS_READ, rf, (long)rbuf, BIGLEN);
    nsi(SYS_CLOSE, rf, 0, 0);
    if (n != BIGLEN) fail("short readback");
    for (int i = 0; i < BIGLEN; i++)
        if (rbuf[i] != pat[i]) fail("readback mismatch");

    wr("PRADYOS_BIGWRITE_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
