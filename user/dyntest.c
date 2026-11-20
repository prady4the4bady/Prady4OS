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
 * argc is read from the stack before any prologue runs: _start is naked and
 * hands %rsp to dyn_main (DDR-823: a C _start's prologue has already moved it).
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
    for (;;)
        nsi(SYS_EXIT, 0, 0, 0);
}

__attribute__((noreturn, used)) void dyn_main(const uint64_t *sp) {
    if (sp[0] >= 2)                        /* argc: execve'd as a child */
        child();
    parent();
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__("mov %rsp, %rdi\n\t"
            "and $-16, %rsp\n\t"
            "call dyn_main\n\t"
            "ud2");
}
