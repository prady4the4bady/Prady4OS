/* user/accmodetest.c — ring-3 fd access-mode probe (DDR-1182).
 *
 * Spawned with its root_mnt on the SFS volume (the writable CoW root).
 *
 * Before DDR-1182 the kernel never read the access mode back out of an fd's
 * open flags, so every FD_VFS fd was readable AND writable whatever open() was
 * asked for. Each arm below opens with one mode and attempts the operation that
 * mode forbids, on every path that reaches the file: write, read, io_uring
 * OP_WRITE / OP_READ, ftruncate, open-time O_TRUNC, a dup2'd copy, and mmap.
 *
 * Every arm gets its OWN 8-byte file, so a mutant that lets one forbidden
 * operation through (and changes that file) cannot move another arm's result.
 * Writes that are meant to be refused are issued at end-of-file (lseek
 * SEEK_END first): on the unfixed tree they then succeed as plain appends,
 * instead of failing for an unrelated reason (SFS has no mid-file overwrite,
 * DDR-1089) and printing some other negative that could be mistaken for a
 * refusal. That keeps every arm red on the unfixed tree.
 *
 * Each arm prints the raw value the kernel returned; the gate matches the
 * exact line. Any setup step that fails prints "ACCMODE FAIL" (forbidden).
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "pradyos.h"

#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_CREAT   0x40
#define O_TRUNC   0x200

#define SEEK_SET  0
#define SEEK_END  2

#define PROT_READ   0x1
#define MAP_PRIVATE 0x02

#define IORING_OP_READ  0
#define IORING_OP_WRITE 1

#define nsi pradyos_syscall

/* SYS_MMAP is the six-argument POSIX form (DDR-877). */
static inline long nsi6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* Each arm line leaves in ONE write: other processes print to the same
 * serial port, and a line written in pieces can be split by their output
 * (mut/accmode-M7 run, job 118899269503: "size=PRADYOS_FPU_OK"). */
static char g_line[160];
static long g_ll;

static void flush_line(void) {
    if (g_ll) nsi(SYS_WRITE, 1, (long)g_line, g_ll);
    g_ll = 0;
}

static void wr(const char *s) {
    for (; *s; s++) {
        if (g_ll == (long)sizeof g_line) flush_line();
        g_line[g_ll++] = *s;
        if (*s == '\n') flush_line();
    }
}

static void wrl(long v) {
    char b[24];
    int k = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    char d[20];
    int nd = 0;
    do { d[nd++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[k++] = '-';
    while (nd) b[k++] = d[--nd];
    b[k] = 0;
    wr(b);
}

__attribute__((noreturn)) static void fail(const char *why) {
    wr("ACCMODE FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Create `path` holding exactly 8 bytes, through an O_WRONLY fd. */
static void mk(const char *path) {
    long fd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (fd < 0) fail("create");
    if (nsi(SYS_WRITE, fd, (long)"ACCMODE!", 8) != 8) fail("initial write");
    nsi(SYS_CLOSE, fd, 0, 0);
}

static long openf(const char *path, long flags) {
    long fd = nsi(SYS_OPEN, (long)path, flags, 0);
    if (fd < 0) fail("open");
    return fd;
}

/* The file's length as a FRESH open sees it. An fd opened earlier caches the
 * size in its own vfs_file, so it would not see an unlink + re-create. */
static long fsize(const char *path) {
    char st[144];                         /* struct stat; st_size at +48 */
    for (int i = 0; i < 144; i++) st[i] = 0;
    long fd = openf(path, O_RDONLY);
    if (nsi(SYS_FSTAT, fd, (long)st, 0) != 0) fail("fstat");
    nsi(SYS_CLOSE, fd, 0, 0);
    long sz = 0;
    for (int i = 7; i >= 0; i--)
        sz = (sz << 8) | (unsigned char)st[48 + i];
    return sz;
}

static void put_sqe(char *ring, int i, int op, long fd, char *buf, int len) {
    char *s = ring + 32 + 32 * i;         /* sqes[i]: after the 32-byte header */
    for (int k = 0; k < 32; k++) s[k] = 0;
    s[0] = (char)op;
    *(int *)(s + 4)            = (int)fd;
    *(unsigned long *)(s + 8)  = (unsigned long)buf;
    *(unsigned *)(s + 16)      = (unsigned)len;
    *(unsigned long *)(s + 24) = (unsigned long)(i + 1);
}

static int cqe_res(char *ring, int i) {
    return *(int *)(ring + 32 + 32 * 8 + 16 * i + 8);  /* cqes[i].res */
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    char buf[16];

    /* A: write on an O_RDONLY fd. */
    mk("/ACCA.TXT");
    long ro = openf("/ACCA.TXT", O_RDONLY);
    nsi(SYS_LSEEK, ro, 0, SEEK_END);
    long a = nsi(SYS_WRITE, ro, (long)"xxxx", 4);
    nsi(SYS_CLOSE, ro, 0, 0);
    wr("PRADYOS_ACCMODE_A ro_write="); wrl(a); wr("\n");

    /* B: read on an O_WRONLY fd. */
    mk("/ACCB.TXT");
    long wo = openf("/ACCB.TXT", O_WRONLY);
    long b = nsi(SYS_READ, wo, (long)buf, 8);
    nsi(SYS_CLOSE, wo, 0, 0);
    wr("PRADYOS_ACCMODE_B wo_read="); wrl(b); wr("\n");

    /* C: the io_uring paths, which reach vfs_write/vfs_read directly and not
     * through the write()/read() handlers above. */
    mk("/ACCC.TXT");
    long cro = openf("/ACCC.TXT", O_RDONLY);
    long cwo = openf("/ACCC.TXT", O_WRONLY);
    nsi(SYS_LSEEK, cro, 0, SEEK_END);
    long rv = nsi(SYS_IO_URING_SETUP, 8, 0, 0);
    if (rv < 0) fail("io_uring setup");
    char *ring = (char *)rv;
    char wbuf[4] = { 'y', 'y', 'y', 'y' };
    put_sqe(ring, 0, IORING_OP_WRITE, cro, wbuf, 4);
    put_sqe(ring, 1, IORING_OP_READ,  cwo, buf, 4);
    if (nsi(SYS_IO_URING_ENTER, rv, 2, 0) != 2) fail("io_uring enter");
    nsi(SYS_CLOSE, cro, 0, 0);
    nsi(SYS_CLOSE, cwo, 0, 0);
    wr("PRADYOS_ACCMODE_C ring_ro_write="); wrl(cqe_res(ring, 0));
    wr(" ring_wo_read="); wrl(cqe_res(ring, 1)); wr("\n");

    /* D: ftruncate on an O_RDONLY fd; the file must keep its 8 bytes. */
    mk("/ACCD.TXT");
    long dro = openf("/ACCD.TXT", O_RDONLY);
    long d = nsi(SYS_FTRUNCATE, dro, 0, 0);
    nsi(SYS_CLOSE, dro, 0, 0);
    wr("PRADYOS_ACCMODE_D ro_ftrunc="); wrl(d);
    wr(" size="); wrl(fsize("/ACCD.TXT")); wr("\n");

    /* E: open-time checks. Access mode 3 means nothing; O_RDONLY|O_TRUNC
     * would modify a file through a read-only open. */
    mk("/ACCE.TXT");
    long e3 = nsi(SYS_OPEN, (long)"/ACCE.TXT", 3, 0);
    if (e3 >= 0) nsi(SYS_CLOSE, e3, 0, 0);
    long et = nsi(SYS_OPEN, (long)"/ACCE.TXT", O_RDONLY | O_TRUNC, 0);
    if (et >= 0) nsi(SYS_CLOSE, et, 0, 0);
    wr("PRADYOS_ACCMODE_E acc3="); wrl(e3);
    wr(" ro_trunc="); wrl(et);
    wr(" size="); wrl(fsize("/ACCE.TXT")); wr("\n");

    /* F: a dup2 copy keeps the source's mode. */
    mk("/ACCF.TXT");
    long fro = openf("/ACCF.TXT", O_RDONLY);
    if (nsi(SYS_DUP2, fro, 40, 0) != 40) fail("dup2");
    nsi(SYS_LSEEK, 40, 0, SEEK_END);
    long f = nsi(SYS_WRITE, 40, (long)"xxxx", 4);
    nsi(SYS_CLOSE, 40, 0, 0);
    nsi(SYS_CLOSE, fro, 0, 0);
    wr("PRADYOS_ACCMODE_F dup_ro_write="); wrl(f); wr("\n");

    /* G: mapping a file reads it, so an O_WRONLY fd may not be mapped. */
    mk("/ACCG.TXT");
    long gwo = openf("/ACCG.TXT", O_WRONLY);
    long g = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, gwo, 0);
    nsi(SYS_CLOSE, gwo, 0, 0);
    wr("PRADYOS_ACCMODE_G wo_mmap="); wrl(g < 0 ? g : 0x7fff); wr("\n");

    /* H (regression): an O_RDWR fd still writes and reads. */
    mk("/ACCH.TXT");
    long hrw = openf("/ACCH.TXT", O_RDWR);
    nsi(SYS_LSEEK, hrw, 0, SEEK_END);
    long hw = nsi(SYS_WRITE, hrw, (long)"zzzz", 4);
    nsi(SYS_LSEEK, hrw, 0, SEEK_SET);
    long hr = nsi(SYS_READ, hrw, (long)buf, 4);
    nsi(SYS_CLOSE, hrw, 0, 0);
    wr("PRADYOS_ACCMODE_H rw_write="); wrl(hw);
    wr(" rw_read="); wrl(hr); wr("\n");

    wr("PRADYOS_ACCMODE_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
