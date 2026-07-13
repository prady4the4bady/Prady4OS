/* user/rootmounttest.c — per-process root-mount probe (DDR-739).
 *
 * Spawned with its root_mnt set to the ext4 volume (not the global FAT default).
 * Two-sided proof that SYS_OPEN resolved against the SELECTED root:
 *   - /EXT4.TXT opens and reads "ext4 read works" (exists only on ext4), AND
 *   - /HELLO.TXT fails to open (exists only on the FAT default root).
 * Both -> PRADYOS_ROOTMOUNT_OK; otherwise "ROOTMOUNT FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE  6
#define SYS_READ   5
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
    wr("ROOTMOUNT FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    /* (a) /EXT4.TXT resolves on the ext4 root and holds the known content. */
    long fd = nsi(SYS_OPEN, (long)"/EXT4.TXT", 0, 0);
    if (fd < 0) fail("EXT4.TXT not found on ext4 root");
    char buf[32];
    long n = nsi(SYS_READ, fd, (long)buf, 15);
    nsi(SYS_CLOSE, fd, 0, 0);
    const char *want = "ext4 read works";
    if (n < 15) fail("EXT4.TXT short read");
    for (int i = 0; i < 15; i++)
        if (buf[i] != want[i]) fail("EXT4.TXT content mismatch");

    /* (b) /HELLO.TXT lives only on the FAT default root — it must NOT resolve
     * here, proving this process is NOT using the default root. */
    long hf = nsi(SYS_OPEN, (long)"/HELLO.TXT", 0, 0);
    if (hf >= 0) { nsi(SYS_CLOSE, hf, 0, 0); fail("HELLO.TXT resolved (using FAT root)"); }

    wr("PRADYOS_ROOTMOUNT_OK ext4\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
