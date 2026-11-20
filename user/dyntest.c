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
#define SYS_EXECVE 14
#define SYS_FORK   15
#define SYS_WAIT4  16

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_PRIVATE    0x02
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

/* DDR-1189 A2 P2-5: arm C. */
__attribute__((noreturn)) static void pie_phase(void) {
    static const char pie[] = "/DYNPIE.ELF";
    static const char pint[] = "/DYNPINT.ELF";
    static const char ptxt[] = "/DYNPTXT.ELF";
    for (int i = 0; i < DYN_CHILDREN; i++)
        run_one(pie, "dynpie", 0);
    run_one(pint, "dynpint", "interp");
    run_one(ptxt, "dynptxt", "textrel");
    wr("PRADYOS_DYN_PIE_DONE n=4\n");
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
