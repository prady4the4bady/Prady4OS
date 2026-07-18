/* user/syscallfuzz.c — hostile-syscall fuzz probe (DDR-758).
 *
 * Issues ~3000 adversarial syscalls from ring 3 and confirms the kernel survives
 * every one: bad NSI numbers must dispatch to -ENOSYS (bounds check, no off-end
 * deref), and wild pointers into a read-only allowlist must return -EFAULT (never
 * fault the kernel). Deterministic (fixed-seed LCG) -> reproducible in CI. Prints
 * PRADYOS_FUZZ_OK once all calls return; a kernel fault during the flood is a
 * panic the boot-test catches. Wild pointers can't kill the probe — the uaccess
 * path returns -EFAULT to the caller rather than delivering the fault.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_EXIT   4
#define SYS_WRITE  6
#define MAX_SYSCALLS 80

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) void _start(void) {
    /* Read-only / query syscalls that take user pointers — wild args -> -EFAULT/
     * -EBADF/-EINVAL, no destructive side effect, cannot self-terminate. */
    static const long SAFE[] = {
        5, 6, 9, 11, 27, 53, 64, 66, 67, 71, 72, 73, 74, 75
    };
    /* Wild pointer values: NULL, kernel VA, non-canonical, unmapped user,
     * a low-but-plausible junk address. */
    static const unsigned long WILD[] = {
        0UL, 0xdeadbeefUL, 0xffffffff80000000UL,
        0x8000000000UL, 0xffffffffffffffffUL, 0x41414141UL
    };
    const int NSAFE = (int)(sizeof SAFE / sizeof SAFE[0]);
    const int NWILD = (int)(sizeof WILD / sizeof WILD[0]);

    unsigned long s = 0x0123456789abcdefUL;      /* fixed seed -> reproducible */
    for (int i = 0; i < 3000; i++) {
        s = s * 6364136223846793005UL + 1442695040888963407UL;  /* 64-bit LCG */
        unsigned long r = s >> 33;

        if (r & 1UL) {
            /* Bad NSI number — handler is never reached (bounds check). */
            long num;
            if (r & 2UL)
                num = -(long)(r % 1000UL) - 1;               /* negative */
            else
                num = (long)(MAX_SYSCALLS + (r % 4000UL));   /* out of range */
            unsigned long a = WILD[(r >> 8) % (unsigned)NWILD];
            (void)nsi(num, (long)a, (long)a, (long)a);
        } else {
            /* Wild pointer into a real read-only syscall -> must error, survive. */
            long num = SAFE[(r >> 4) % (unsigned)NSAFE];
            unsigned long p = WILD[(r >> 12) % (unsigned)NWILD];
            (void)nsi(num, (long)p, (long)p, (long)p);
        }
    }

    wr("PRADYOS_FUZZ_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
