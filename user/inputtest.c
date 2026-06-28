/* user/inputtest.c — ring-3 keyboard input reader (Layer 7, DDR-703).
 *
 * Announces readiness (PRADYOS_INPUT_WAIT), then polls SYS_INPUT_POLL for key
 * bytes the kernel collected from IRQ1. On the first key it prints
 * PRADYOS_INPUT_OK <char>; times out cleanly if no input arrives.
 */
#include <stdio.h>

#define SYS_EXIT       4
#define SYS_YIELD      3
#define SYS_INPUT_POLL 46

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    printf("PRADYOS_INPUT_WAIT\n");
    fflush(stdout);

    char buf[32];
    /* Poll for up to ~30 s (yielding so other work runs). The gate injects keys
     * via QEMU sendkey shortly after seeing PRADYOS_INPUT_WAIT. */
    for (long i = 0; i < 2000000; i++) {
        long n = nsi(SYS_INPUT_POLL, (long)buf, (long)sizeof buf, 0);
        if (n > 0) {
            char c = buf[0];
            /* Print printable chars directly; report control bytes by code. */
            if (c >= 0x20 && c < 0x7f)
                printf("PRADYOS_INPUT_OK %c\n", c);
            else
                printf("PRADYOS_INPUT_OK code=%d\n", (int)(unsigned char)c);
            fflush(stdout);
            nsi(SYS_EXIT, 0, 0, 0);
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    printf("PRADYOS_INPUT_TIMEOUT\n");
    fflush(stdout);
    nsi(SYS_EXIT, 1, 0, 0);
    return 0;
}
