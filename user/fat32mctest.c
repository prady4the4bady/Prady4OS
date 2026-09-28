/* user/fat32mctest.c — FAT32 multi-cluster read regression probe (DDR-973).
 *
 * ADR-024 §D5 deferred init-driven execve-respawn of PRISM on the report that
 * "execve of a large musl-C ELF read from FAT32 corrupts the loaded image",
 * root-caused at the time only as "most likely FAT32 multi-cluster reads for
 * large files". That attribution was never measured — it was written when
 * execve had only ever been exercised with the 4 KiB *asm* EXECTEST, and the
 * defect was never reproduced afterwards. This probe is the measurement, and
 * it is permanent: it fails loudly if a multi-cluster read ever does corrupt.
 *
 * The FAT volume is mkfs.fat -F32 over 64 MiB, which lands on 1 sector per
 * cluster — a 512-byte cluster. So /BIGPAT.BIN (65,536 B) spans 128 clusters
 * and /CMUSL.ELF (30,488 B) spans 60. Every read below therefore crosses many
 * cluster boundaries, and the arm-B offsets sit on them deliberately.
 *
 * Arm A  sequential whole-file scan, 4 KiB at a time: every one of the 65,536
 *        bytes must equal want_at() below. A chain walk that repeats, skips or
 *        transposes a cluster shifts the pattern and is caught at the first
 *        wrong byte, whose absolute offset is printed. Read want_at()'s comment
 *        before changing the pattern — the obvious one makes this arm vacuous.
 * Arm B  short reads straddling a cluster boundary, reached by lseek so each
 *        one re-walks the chain from the head — the O(n) walk fat32_read does
 *        on every call, which is what a deep-chain defect would break.
 * Arm C  execve("/CMUSL.ELF"): sys_execve reads the whole 30,488-byte image in
 *        ONE vfs_read and jumps to it. This is the exact ADR-024 case, end to
 *        end. It does not return; PRADYOS_FAT32_MC_OK is printed first, and
 *        the gate's denominator for the marker cmusl prints is stated there.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (DDR-826/user.ld).
 * The 4 KiB read buffer is a stack local and stays inside the 8 eager stack
 * pages of ADR-038 — vmm_user_range_ok validates syscall pointers WITHOUT
 * faulting them in, so a buffer past the eager window would be rejected. That
 * is also why arm A chunks rather than reading 64 KiB in one call; it costs
 * nothing, because fat32_read re-walks the chain from the head on EVERY call,
 * so the 4 KiB read at offset 61440 still traverses all 120 clusters ahead of
 * it. Arm C is where a single large read is exercised.
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_EXIT     4
#define SYS_READ     5
#define SYS_WRITE    6
#define SYS_OPEN     7
#define SYS_CLOSE    8
#define SYS_LSEEK   10
#define SYS_EXECVE  14

#define O_RDONLY     0x0
#define SEEK_SET     0

#define PAT_BYTES    65536L     /* /BIGPAT.BIN — 128 clusters                  */
#define CHUNK        4096L      /* 8 clusters per read; inside the eager stack */

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

/* Decimal, into a caller-supplied stack buffer (no writable globals). */
static void wrdec(long v) {
    char b[24];
    int i = (int)sizeof b;
    b[--i] = 0;
    if (v < 0) { wr("-"); v = -v; }
    if (v == 0) b[--i] = '0';
    while (v > 0) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    wr(&b[i]);
}

/* The byte the pattern generator wrote at absolute file offset `off`:
 *
 *     byte n = (7n + 3 + 31*(n >> 8)) & 0xFF
 *
 * The 7n+3 term is the texture. The 31*(n>>8) term is what makes the file
 * usable as a chain-walk oracle, and it is NOT decoration -- see DDR-973 sec.6.
 * Plain (7n+3)&0xFF has period 256, and this volume's clusters are 512 B, so
 * under that pattern EVERY cluster holds identical bytes and a chain that
 * repeats, skips or transposes a cluster reads back perfectly. A mutant that
 * re-read cluster 64 instead of advancing passed the gate on the first cut of
 * this probe. 31 is invertible mod 256, so 31*k stamps each of the 256 blocks
 * in the 64 KiB file distinctly; stamping per 256-byte block rather than per
 * cluster keeps the oracle independent of the volume's sectors-per-cluster. */
static unsigned char want_at(long off) {
    return (unsigned char)((7 * off + 3 + 31 * (off >> 8)) & 0xFF);
}

__attribute__((noreturn)) static void fail(const char *why, long off) {
    wr("FAT32MC FAIL: ");
    wr(why);
    wr(" off=");
    wrdec(off);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) static void fail_byte(long off, unsigned char got) {
    wr("FAT32MC FAIL: pattern mismatch off=");
    wrdec(off);
    wr(" got=");
    wrdec((long)got);
    wr(" want=");
    wrdec((long)want_at(off));
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* One boundary-straddling read: seek, read, verify every byte, and hold the
 * gate's own expectation about the length (a read running off EOF must come
 * back short, not wrapped). A failure exits the process outright via
 * fail()/fail_byte(); the fd goes with it. */
static void straddle(long fd, char *buf, long off, long len) {
    long expect = (off + len > PAT_BYTES) ? (PAT_BYTES - off) : len;
    if (nsi(SYS_LSEEK, fd, off, SEEK_SET) != off) fail("lseek", off);
    long n = nsi(SYS_READ, fd, (long)buf, len);
    if (n != expect) fail("straddle read length at", off);
    for (long i = 0; i < n; i++)
        if ((unsigned char)buf[i] != want_at(off + i))
            fail_byte(off + i, (unsigned char)buf[i]);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char *pat = "/BIGPAT.BIN";
    char buf[CHUNK];

    /* ---- Arm A: sequential whole-file scan ------------------------------ */
    long fd = nsi(SYS_OPEN, (long)pat, O_RDONLY, 0);
    if (fd < 0) fail("open /BIGPAT.BIN", fd);

    long pos = 0;
    while (pos < PAT_BYTES) {
        long want = PAT_BYTES - pos;
        if (want > CHUNK) want = CHUNK;
        long n = nsi(SYS_READ, fd, (long)buf, want);
        if (n != want) { nsi(SYS_CLOSE, fd, 0, 0); fail("short sequential read at", pos); }
        for (long i = 0; i < n; i++)
            if ((unsigned char)buf[i] != want_at(pos + i)) {
                nsi(SYS_CLOSE, fd, 0, 0);
                fail_byte(pos + i, (unsigned char)buf[i]);
            }
        pos += n;
    }
    /* Past EOF the read must be empty, not a wrapped or repeated cluster. */
    if (nsi(SYS_READ, fd, (long)buf, 16) != 0) { nsi(SYS_CLOSE, fd, 0, 0); fail("read past EOF returned data", PAT_BYTES); }

    /* ---- Arm B: boundary straddles, each re-walking the chain ----------- */
    /* Offsets 511/1023/4095/32767 sit one byte before a cluster edge, so each
     * read spans two clusters; 510 spans one with a byte to spare on each side;
     * 65530 runs off the end and must come back short by exactly 10.
     *
     * Written as six calls, not a table: a `static const` array lands in
     * .lrodata under this build's code model, which user/user.ld does not name
     * (it matches .rodata*) and which therefore only ends up inside the PT_LOAD
     * by lld's orphan placement. Every other probe in user/ avoids that by
     * having no such array, and this one is not the place to start depending on
     * it -- nor to edit a linker script 40+ shipped probes share. */
    straddle(fd, buf,   511,  2);
    straddle(fd, buf,   510,  4);
    straddle(fd, buf,  1023,  3);
    straddle(fd, buf,  4095,  2);
    straddle(fd, buf, 32767,  2);
    straddle(fd, buf, 65530, 16);   /* short by 10 at EOF */

    nsi(SYS_CLOSE, fd, 0, 0);

    { uline u; ul_init(&u);                       /* DDR-1056: ONE write */
      ul_s(&u, "PRADYOS_FAT32_MC_OK bytes="); ul_d(&u, PAT_BYTES);
      ul_s(&u, " clusters=128 straddles=6\n"); wr(ul_end(&u)); }

    /* ---- Arm C: the ADR-024 case itself -------------------------------- */
    /* sys_execve reads all 30,488 bytes of /CMUSL.ELF in one vfs_read and
     * enters it. On success cmusl prints PRADYOS_MUSL_OK and this thread never
     * comes back here; the gate asserts that marker appears TWICE, because the
     * boot already runs one SFS-loaded copy (main.c CMUSL.ELF) — one occurrence
     * means this exec did not happen. */
    nsi(SYS_EXECVE, (long)"/CMUSL.ELF", 0, 0);

    /* Only reached if execve failed. */
    fail("execve /CMUSL.ELF returned", 0);
}
