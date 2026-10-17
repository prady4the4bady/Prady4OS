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

/* DDR-1089: the exact values the two refusal arms pin. Asserting only "returns
 * negative" is not enough and this repo already established that for a sibling
 * syscall -- ftrunctest.c:121 records a mutant that removed a sign check and
 * still came back negative, so a sign-only assertion passed while the guard it
 * claimed to test was gone. BOTH of these were -EIO before this change:
 * sfs_write returned a bare -1 for seven unrelated conditions and
 * fd_write_user flattened every negative to -EIO. */
#define ENOSYS_    38   /* mid-file overwrite: sfs_write is append-only         */
#define EFBIG_     27   /* file full: 4 inline extents is the SFS ceiling       */

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

/* DDR-1089: the two refusal arms REPORT and let the gate JUDGE (DDR-1020's
 * rule: every fail() before the print silently removes an arm -- measured here,
 * because with fail() the M1 mutant died at arm A and arm B was never reached).
 * Both values go out on ONE line via one write(2) -- DDR-1056's uline rule, so a
 * concurrent printer cannot splice the sentinel the gate greps for. */
static char *dec(char *p, long v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

__attribute__((noreturn)) static void fail(const char *why) {
    wr("BIGWRITE FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
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

    /* DDR-1089 ARM A — the append-only refusal names itself.
     *
     * /BIG.TXT now holds 8192 bytes. Reopening it WRITE-ONLY (no O_CREAT, no
     * truncation) leaves the fd offset at 0 while the inode's size is 8192, so
     * sfs_write's `off != in->size` fires BEFORE it looks at the length -- which
     * is why PRE_LAUNCH_CHECKLIST sec.4.9 saw the same refusal for a longer AND
     * an equal-length payload, and why unlink+recreate succeeded (that resets
     * the size to 0). Pinned to the EXACT value: -EIO, the old answer, is also
     * negative, so `< 0` would pass on the unfixed tree. */
    long rw = nsi(SYS_OPEN, (long)"/BIG.TXT", O_WRONLY, 0);
    if (rw < 0) fail("reopen for rewrite");
    long rc = nsi(SYS_WRITE, rw, (long)pat, 64);
    nsi(SYS_CLOSE, rw, 0, 0);

    /* DDR-1089 ARM B — the file-full refusal is a DIFFERENT number.
     *
     * Four separate 4096-byte writes are four extents (fd_write_user's chunk is
     * 4096, so one vfs_write per call); the fifth has nowhere to go. Before this
     * change arm A and arm B returned the SAME value, so the pair is what proves
     * the split rather than either arm alone. */
    long ff = nsi(SYS_OPEN, (long)"/EXT5.TXT", O_CREAT | O_WRONLY, 0);
    if (ff < 0) fail("O_CREAT open EXT5");
    for (int e = 0; e < 4; e++)
        if (nsi(SYS_WRITE, ff, (long)pat, 4096) != 4096) fail("extent fill short");
    long fifth = nsi(SYS_WRITE, ff, (long)pat, 4096);
    nsi(SYS_CLOSE, ff, 0, 0);

    /* One line, both values, printed UNCONDITIONALLY. The gate pins the exact
     * pair, so neither arm can mask the other and a wrong value is visible in
     * the capture rather than hidden behind an early exit. -EIO (-5) was the old
     * answer for BOTH, and it is negative too -- which is why `< 0` would pass on
     * the unfixed tree (ftrunctest.c:121 records that same trap for ftruncate). */
    {   char line[64]; char *p = line;
        const char *tag = "PRADYOS_BIGWRITE_ERRNO rw=";
        for (int i = 0; tag[i]; i++) *p++ = tag[i];
        p = dec(p, rc);
        *p++ = ' '; *p++ = 'e'; *p++ = 'x'; *p++ = 't'; *p++ = '5'; *p++ = '=';
        p = dec(p, fifth);
        *p++ = '\n';
        nsi(SYS_WRITE, 1, (long)line, p - line);
    }
    (void)ENOSYS_; (void)EFBIG_;
    wr("PRADYOS_BIGWRITE_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
