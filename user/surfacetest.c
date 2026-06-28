/* user/surfacetest.c — a minimal client window (Layer 7, DDR-706).
 *
 * Creates a 64x64 surface, maps it, fills it green, and commits it at (100,100) —
 * then stays alive so its surface buffer persists for the compositor to composite.
 * Prints PRADYOS_SURFACE_CLIENT_OK on success.
 */
#include <stdio.h>

#define SYS_EXIT           4
#define SYS_YIELD          3
#define SYS_SURFACE_CREATE 48
#define SYS_SURFACE_MAP    49
#define SYS_SURFACE_COMMIT 50

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    long id = nsi(SYS_SURFACE_CREATE, 64, 64, 0);
    if (id < 0) {
        printf("PRADYOS_SURFACE_CLIENT_FAIL create=%ld\n", id);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    long va = nsi(SYS_SURFACE_MAP, id, 0, 0);
    if (va < 0) {
        printf("PRADYOS_SURFACE_CLIENT_FAIL map=%ld\n", va);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    unsigned char *s = (unsigned char *)va;
    for (unsigned i = 0; i < 64u * 64u; i++) {      /* fill green (BGRA) */
        s[i * 4 + 0] = 0x40; s[i * 4 + 1] = 0xE0;
        s[i * 4 + 2] = 0x40; s[i * 4 + 3] = 0xFF;
    }
    nsi(SYS_SURFACE_COMMIT, id, 100, 100);
    printf("PRADYOS_SURFACE_CLIENT_OK id=%ld\n", id);
    fflush(stdout);

    /* Keep the surface alive (a window stays open) — destroy is deferred. */
    for (;;)
        nsi(SYS_YIELD, 0, 0, 0);
    return 0;
}
