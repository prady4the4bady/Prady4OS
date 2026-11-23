/* user/dyntest.c — DDR-1189 Amendment 1 (C1.6 P1): user ASLR probe for
 * smoke-dynlink arms A and B.
 *
 * One image, two roles, told apart by argc:
 *   - the boot instance (elf_load gives argc=1) forks four children, and each
 *     execve()s /DYNTEST.ELF with argv[1] = "child". It waits for each in turn,
 *     then prints PRADYOS_DYN_DONE n=4.
 *   - a child (argc=2) prints its layout and exits:
 *       PRADYOS_DYN_L stack=0x... mmap=0x...
 *     stack = the address of a local in its entry function; mmap = the address
 *     a fresh one-page anonymous mmap returns.
 *
 * Every sample comes from execve, so the gate tests execve's commit (where the
 * drawn mmap base is stored), not only the boot path (mutant D2b).
 *
 * DDR-1189 Amendment 2 (C1.6 P2), arm C: after PRADYOS_DYN_DONE the boot
 * instance forks four children that execve /DYNPIE.ELF (a static PIE; each
 * prints PRADYOS_DYN_P), then two that execve the images P2 must refuse,
 * /DYNPINT.ELF (PT_INTERP) and /DYNPTXT.ELF (a text relocation). Each of those
 * two prints `PRADYOS_DYN_R <what> rc=<execve's return>` when execve returns.
 * Last, PRADYOS_DYN_PIE_DONE n=4.
 *
 * argc is read from the stack before any prologue runs: _start is an assembly
 * stub that hands %rsp to dyn_main (DDR-823: a C _start's prologue has
 * already moved it).
 * No writable globals: this probe links as one R+X segment.
 * QEMU only, not verified on real hardware.
 */
#include <stdint.h>

#define SYS_EXIT    4
#define SYS_WRITE   6
#define SYS_MMAP   12
#define SYS_MUNMAP 13
#define SYS_MEMINFO 74
#define SYS_EXECVE 14
#define SYS_FORK   15
#define SYS_WAIT4  16
#define SYS_OPEN    7
#define SYS_CLOSE   8
#define SYS_MPROTECT 97

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define O_RDONLY       0
#define O_RDWR         2
#define MAP_ANONYMOUS  0x20

#define DYN_CHILDREN 4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

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

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void die(const char *why) {
    wr("DYNTEST FAIL: ");
    wr(why);
    wr("\n");
    for (;;)
        nsi(SYS_EXIT, 1, 0, 0);
}

/* "0x" and 16 hex digits into out[0..17], NUL at out[18]. */
static void hex64(char *out, uint64_t v) {
    static const char dig[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = dig[(v >> (60 - 4 * i)) & 0xfu];
    out[18] = 0;
}

__attribute__((noreturn)) static void child(void) {
    volatile uint64_t here = 0;
    uint64_t stack = (uint64_t)(uintptr_t)&here;
    long m = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m <= 0)
        die("child mmap");
    *(volatile uint8_t *)m = 1;            /* the page is really there */
    char hs[19], hm[19];
    hex64(hs, stack);
    hex64(hm, (uint64_t)m);
    wr("PRADYOS_DYN_L stack=");
    wr(hs);
    wr(" mmap=");
    wr(hm);
    wr("\n");
    for (;;)
        nsi(SYS_EXIT, 0, 0, 0);
}

/* A signed decimal into out (at least 21 bytes), NUL-terminated. */
static void dec(char *out, long v) {
    char t[20];
    int n = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    int o = 0;
    if (v < 0)
        out[o++] = '-';
    while (n)
        out[o++] = t[--n];
    out[o] = 0;
}

/* Fork; the child execve()s path with argv {a0}. With `what` set, the image
 * must be refused: the child prints PRADYOS_DYN_R <what> rc=<rc> and exits 0.
 * The parent waits and requires exit status 0 either way. */
static void run_one(const char *path, const char *a0, const char *what) {
    long pid = nsi(SYS_FORK, 0, 0, 0);
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        const char *argv[2];               /* on the stack: no writable globals */
        const char *envp[1];
        argv[0] = a0;
        argv[1] = 0;
        envp[0] = 0;
        long rc = nsi(SYS_EXECVE, (long)path, (long)argv, (long)envp);
        if (!what)
            die("execve dynpie returned");
        char d[24];
        dec(d, rc);
        wr("PRADYOS_DYN_R ");
        wr(what);
        wr(" rc=");
        wr(d);
        wr("\n");
        for (;;)
            nsi(SYS_EXIT, 0, 0, 0);
    }
    int st = -1;
    if (nsi(SYS_WAIT4, pid, (long)&st, 0) != pid)
        die("wait4");
    if (st != 0)
        die("a child exited non-zero");
}

/* " key=<v>" */
static void kv(const char *key, long v) {
    char d[24];
    dec(d, v);
    wr(" ");
    wr(key);
    wr("=");
    wr(d);
}

/* DDR-1189 A3 P3a-1 (L1): arm F. One line of return codes; the gate checks
 * each rule as its own sub-arm. A positive address is printed as 1. */
static void wx_phase(void) {
    static const char self[] = "/DYNTEST.ELF";
    long ro = nsi(SYS_OPEN, (long)self, O_RDONLY, 0);
    long rw = nsi(SYS_OPEN, (long)self, O_RDWR, 0);
    if (ro < 0 || rw < 0)
        die("open /DYNTEST.ELF");
    /* F1: write and execute together, even from a read-only fd. */
    long wx = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, ro, 0);
    /* F2: anonymous, and MAP_SHARED, executable mappings keep -EINVAL. */
    long anonx = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long sharedx = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, ro, 0);
    /* F3: an executable mapping only from a read-only fd. */
    long rwfd = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, rw, 0);
    /* F0: the legal path -- a read-only file, MAP_PRIVATE, executable. */
    long filex = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, ro, 0);
    /* F4: mprotect never adds X to anonymous memory (mapped read-only, so
     * only the "came from a read-only file" rule refuses it). */
    long anon = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon <= 0)
        die("F4 setup map");
    long mpanon = nsi(SYS_MPROTECT, anon, 4096, PROT_READ | PROT_EXEC);
    /* F6: L1 names MAP_PRIVATE. Neither flag is not "private": an executable
     * request is refused, and a plain read-only mapping never becomes
     * eligible for X. */
    long noflagx = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_EXEC, 0, ro, 0);
    long nf = nsi6(SYS_MMAP, 0, 4096, PROT_READ, 0, ro, 0);
    if (nf <= 0)
        die("F6 setup map");
    long noflagmp = nsi(SYS_MPROTECT, nf, 4096, PROT_READ | PROT_EXEC);
    /* F0 and F5: a read-only file mapping may become executable while it was
     * never writable; once made writable, never again. */
    long fr = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, ro, 0);
    long frx = fr > 0 ? nsi(SYS_MPROTECT, fr, 4096, PROT_READ | PROT_EXEC) : fr;
    long frw = fr > 0 ? nsi(SYS_MPROTECT, fr, 4096, PROT_READ | PROT_WRITE) : fr;
    long frwx = fr > 0 ? nsi(SYS_MPROTECT, fr, 4096, PROT_READ | PROT_EXEC) : fr;
    nsi(SYS_CLOSE, ro, 0, 0);
    nsi(SYS_CLOSE, rw, 0, 0);
    wr("PRADYOS_DYN_F");
    kv("wx", wx > 0 ? 1 : wx);
    kv("anonx", anonx > 0 ? 1 : anonx);
    kv("sharedx", sharedx > 0 ? 1 : sharedx);
    kv("rwfd", rwfd > 0 ? 1 : rwfd);
    kv("filex", filex > 0 ? 1 : filex);
    kv("mpanon", mpanon);
    kv("frx", frx);
    kv("frw", frw);
    kv("frwx", frwx);
    kv("noflagx", noflagx > 0 ? 1 : noflagx);
    kv("noflagmp", noflagmp);
    wr("\n");
}

struct dyn_meminfo {                       /* SYS_MEMINFO (DDR-752) */
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint32_t page_size;
    uint32_t pad;
};

static long free_pages(void) {
    struct dyn_meminfo mi;
    if (nsi(SYS_MEMINFO, (long)&mi, 0, 0) != 0)
        die("meminfo");
    return (long)mi.free_pages;
}

/* DDR-1189 A3 P3a-2: arm M, MAP_FIXED over the caller's own mappings. One
 * line; the gate checks each rule as its own sub-arm. */
static void fixed_phase(void) {
    static const char self[] = "/DYNTEST.ELF";
    long ro = nsi(SYS_OPEN, (long)self, O_RDONLY, 0);
    if (ro < 0)
        die("open /DYNTEST.ELF");
    /* A two-page span of the file, as musl's ld.so maps a library first. The
     * first byte is read before any replacement, so a stale translation would
     * show the file's 0x7f where the replacement must read 0. */
    long sp = nsi6(SYS_MMAP, 0, 8192, PROT_READ, MAP_PRIVATE, ro, 0);
    if (sp <= 0)
        die("map span");
    volatile unsigned char *p = (volatile unsigned char *)sp;
    long pre = p[0];
    /* M1: a range half inside the span and half in unmapped space. */
    long mixed = nsi6(SYS_MMAP, sp + 4096, 8192, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    /* M0: an anonymous writable page replaces the first page ... */
    long anon = nsi6(SYS_MMAP, sp, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    long anonz = (anon == sp) ? p[0] : -1;
    if (anon == sp)
        p[1] = 0xC3;                           /* written: a "ret" */
    /* M4: written through a replacement, so never executable. */
    long m4 = nsi(SYS_MPROTECT, sp, 4096, PROT_READ | PROT_EXEC);
    /* M0: ... and the file again replaces it, executable from the read-only fd. */
    long back = nsi6(SYS_MMAP, sp, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, ro, 0);
    long backb = (back == sp) ? p[0] : -1;
    /* M2: an anonymous MAP_SHARED page is not the caller's to replace. */
    long sh = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh <= 0)
        die("M2 setup map");
    long shfix = nsi6(SYS_MMAP, sh, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    /* M7: MAP_FIXED|MAP_SHARED at address 0 is refused, not placed as a hint. */
    long fsh0 = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, ro, 0);
    /* M2b: neither is a file MAP_SHARED area (a shm object). */
    long fsh = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_SHARED, ro, 0);
    long fshfix = fsh > 0 ? nsi6(SYS_MMAP, fsh, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) : fsh;
    /* M3: the stack and the image are outside the arena. */
    long stk = nsi6(SYS_MMAP, (long)&ro & ~4095L, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    long img = nsi6(SYS_MMAP, (long)fixed_phase & ~4095L, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    /* M5: wholly unmapped arena space keeps the pre-P3a-2 behaviour. */
    long fr = nsi6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fr <= 0 || nsi(SYS_MUNMAP, fr, 4096, 0) != 0)
        die("M5 setup");
    long fresh = nsi6(SYS_MMAP, fr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    /* M6: a replacement frees the page it replaces. One page replaced in place
     * 256 times leaves the free count where it was; leaking each replaced
     * frame (mutant D9e) would take 256 pages. */
    long lk = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long leak = -1000000;
    if (lk > 0) {
        long f0 = free_pages();
        for (int i = 0; i < 256; i++)
            if (nsi6(SYS_MMAP, lk, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) != lk)
                die("M6 replace");
        leak = f0 - free_pages();
    }
    nsi(SYS_CLOSE, ro, 0, 0);
    wr("PRADYOS_DYN_M");
    kv("pre", pre);
    kv("anon", anon == sp ? 1 : anon);
    kv("anonz", anonz);
    kv("m4", m4);
    kv("back", back == sp ? 1 : back);
    kv("backb", backb);
    kv("mixed", mixed > 0 ? 1 : mixed);
    kv("shfix", shfix > 0 ? 1 : shfix);
    kv("fsh", fsh > 0 ? 1 : fsh);
    kv("fshfix", fshfix > 0 ? 1 : fshfix);
    kv("stack", stk > 0 ? 1 : stk);
    kv("image", img > 0 ? 1 : img);
    kv("fresh", fresh == fr ? 1 : fresh);
    kv("leak", leak);
    kv("fsh0", fsh0 > 0 ? 1 : fsh0);
    wr("\n");
}

/* DDR-1189 A4 M8: map two anonymous writable pages and store to them, replace
 * `cover` bytes from the start with MAP_FIXED (prot, flags, fd), then ask for
 * X on the replaced bytes. Setup dies on failure. *first gets the first byte
 * after the replacement. */
static long m8_try(long cover, int prot, int flags, long fd, long *first) {
    long a = nsi6(SYS_MMAP, 0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a <= 0)
        die("M8 setup map");
    volatile uint8_t *p = (volatile uint8_t *)a;
    p[0] = 0xC3;
    p[4096] = 0xC3;
    if (nsi6(SYS_MMAP, a, cover, prot, flags | MAP_FIXED, fd, 0) != a)
        die("M8 setup replace");
    if (first)
        *first = p[0];
    return nsi(SYS_MPROTECT, a, cover, PROT_READ | PROT_EXEC);
}

/* DDR-1189 A4: arms F7 and F8a-c (the write history is per page, and fork and
 * the COW fault keep it) and M8, M8p/r/w/n (x_ok from a replacement that
 * covers a whole area and meets L1). One line; setup calls die on failure. */
static void a4_phase(void) {
    static const char self[] = "/DYNTEST.ELF";
    long ro = nsi(SYS_OPEN, (long)self, O_RDONLY, 0);
    long rw = nsi(SYS_OPEN, (long)self, O_RDWR, 0);
    if (ro < 0 || rw < 0)
        die("A4 open");
    /* F7: write granted, and used, on page 0 only of a two-page read-only file
     * mapping. Page 1 may still become executable; page 0 never. */
    long f7 = nsi6(SYS_MMAP, 0, 8192, PROT_READ, MAP_PRIVATE, ro, 0);
    if (f7 <= 0 || nsi(SYS_MPROTECT, f7, 4096, PROT_READ | PROT_WRITE) != 0)
        die("F7 setup");
    *(volatile uint8_t *)f7 = 0xC3;
    long f7x1 = nsi(SYS_MPROTECT, f7 + 4096, 4096, PROT_READ | PROT_EXEC);
    long f7x0 = nsi(SYS_MPROTECT, f7, 4096, PROT_READ | PROT_EXEC);
    /* F8: three pages of a read-only file mapping, made writable and written
     * here. At fork, page 0 is read-only again (F8a), pages 1 and 2 are still
     * writable, so COW in the child (F8b). The child writes page 2 first (the
     * COW fault, F8c). It may make none of them executable; it exits with one
     * bit per page that it could. */
    long f8 = nsi6(SYS_MMAP, 0, 3 * 4096, PROT_READ, MAP_PRIVATE, ro, 0);
    if (f8 <= 0 || nsi(SYS_MPROTECT, f8, 3 * 4096, PROT_READ | PROT_WRITE) != 0)
        die("F8 setup");
    volatile uint8_t *q = (volatile uint8_t *)f8;
    q[0] = 0xC3;
    q[4096] = 0xC3;
    q[8192] = 0xC3;
    if (nsi(SYS_MPROTECT, f8, 4096, PROT_READ) != 0)
        die("F8a setup");
    long pid = nsi(SYS_FORK, 0, 0, 0);
    if (pid < 0)
        die("F8 fork");
    if (pid == 0) {
        long bits = 0;
        if (nsi(SYS_MPROTECT, f8, 4096, PROT_READ | PROT_EXEC) != -13)
            bits |= 1;
        if (nsi(SYS_MPROTECT, f8 + 4096, 4096, PROT_READ | PROT_EXEC) != -13)
            bits |= 2;
        q[8192] = 0x90;
        if (nsi(SYS_MPROTECT, f8 + 8192, 4096, PROT_READ | PROT_EXEC) != -13)
            bits |= 4;
        for (;;)
            nsi(SYS_EXIT, bits, 0, 0);
    }
    int st = -1;
    if (nsi(SYS_WAIT4, pid, (long)&st, 0) != pid)
        die("F8 wait4");
    /* M8: the whole area, from the read-only fd, MAP_PRIVATE, no write. */
    long m8b = -1;
    long m8 = m8_try(8192, PROT_READ, MAP_PRIVATE, ro, &m8b);
    /* M8p, M8r, M8w, M8n: page 0 only; a read-write fd; PROT_WRITE; neither
     * MAP_PRIVATE nor MAP_SHARED. None makes the area eligible for X. */
    long m8p = m8_try(4096, PROT_READ, MAP_PRIVATE, ro, 0);
    long m8r = m8_try(8192, PROT_READ, MAP_PRIVATE, rw, 0);
    long m8w = m8_try(8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
    long m8n = m8_try(8192, PROT_READ, 0, ro, 0);
    nsi(SYS_CLOSE, ro, 0, 0);
    nsi(SYS_CLOSE, rw, 0, 0);
    wr("PRADYOS_DYN_W");
    kv("f7x1", f7x1);
    kv("f7x0", f7x0);
    kv("f8", st);
    kv("m8", m8);
    kv("m8b", m8b);
    kv("m8p", m8p);
    kv("m8r", m8r);
    kv("m8w", m8w);
    kv("m8n", m8n);
    wr("\n");
}

/* DDR-1189 A2 P2-5: arm C. */
__attribute__((noreturn)) static void pie_phase(void) {
    static const char pie[] = "/DYNPIE.ELF";
    static const char pint[] = "/DYNPINT.ELF";
    static const char ptxt[] = "/DYNPTXT.ELF";
    static const char d0[] = "/DYND0.ELF";      /* DDR-1189 A3: arm D0 */
    static const char pii[] = "/DYNPII.ELF";
    for (int i = 0; i < DYN_CHILDREN; i++)
        run_one(pie, "dynpie", 0);
    run_one(pint, "dynpint", "interp");
    run_one(ptxt, "dynptxt", "textrel");
    run_one(d0, "dynd0", 0);                    /* via /LIB/LDTEST.SO */
    run_one(pii, "dynpii", "interp2");          /* its interpreter has one */
    wr("PRADYOS_DYN_PIE_DONE n=4\n");
    wx_phase();                            /* DDR-1189 A3: arm F */
    fixed_phase();                         /* DDR-1189 A3: arm M */
    a4_phase();                            /* DDR-1189 A4: arms F7, F8, M8 */
    for (;;)
        nsi(SYS_EXIT, 0, 0, 0);
}

__attribute__((noreturn)) static void parent(void) {
    static const char path[] = "/DYNTEST.ELF";
    static const char a0[] = "dyntest";
    static const char a1[] = "child";
    for (int i = 0; i < DYN_CHILDREN; i++) {
        long pid = nsi(SYS_FORK, 0, 0, 0);
        if (pid < 0)
            die("fork");
        if (pid == 0) {
            const char *argv[3];           /* on the stack: no writable globals */
            const char *envp[1];
            argv[0] = a0;
            argv[1] = a1;
            argv[2] = 0;
            envp[0] = 0;
            nsi(SYS_EXECVE, (long)path, (long)argv, (long)envp);
            die("execve returned");
        }
        int st = -1;
        if (nsi(SYS_WAIT4, pid, (long)&st, 0) != pid)
            die("wait4");
        if (st != 0)
            die("a child exited non-zero");
    }
    wr("PRADYOS_DYN_DONE n=4\n");
    pie_phase();
}

__attribute__((noreturn, used)) void dyn_main(const uint64_t *sp) {
    if (sp[0] >= 2)                        /* argc: execve'd as a child */
        child();
    parent();
}

/* The entry is an assembly stub, not a C function: argc is on the stack at
 * entry and must be read before any prologue runs (DDR-823), and a C entry
 * would need force_align_arg_pointer (ci-start-align-check), whose realigning
 * prologue moves the stack before the body can read it. The stub aligns RSP
 * itself, so dyn_main is entered by a normal call with RSP = 8 (mod 16). */
__asm__(".text\n"
        ".globl _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call dyn_main\n"
        "    ud2\n");
