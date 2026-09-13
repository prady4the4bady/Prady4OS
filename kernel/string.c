/* kernel/string.c — freestanding mem* implementations. */
#include "string.h"

/* DDR-1076 (Group G row 9.6): the real fill lives in
 * arch/x86_64/fast_memset.asm and picks REP STOSB when the CPU advertises
 * ERMS, else REP STOSQ + a byte tail -- sharing DDR-871's ERMS flag rather
 * than probing CPUID a second time, so the two cannot drift.
 *
 * This was a byte-at-a-time loop while memcpy below has routed through
 * fast_memcpy since DDR-871; the asymmetry was paid on EVERY kfree
 * (kheap.c:174 poisons the object) and on every slab growth (kheap.c:326,
 * a full page). Both paths use general-purpose registers only, so neither
 * disturbs FPU/XMM state and the context switch is unaffected.
 *
 * The one thing that can go wrong is the BYTE BROADCAST -- see the .asm
 * header. It is correct for every zero fill and wrong only for a non-zero
 * one, and this tree has exactly three non-zero memset call sites, none of
 * whose bytes any gate verifies. smoke-bench's 0xA7 arm is what covers it. */
void *fast_memset(void *dst, int c, size_t n);

void *memset(void *dst, int c, size_t n) {
    return fast_memset(dst, c, n);
}

/* DDR-871 (item 42): the real copy lives in arch/x86_64/fast_memcpy.asm and
 * picks REP MOVSB when the CPU advertises ERMS, else REP MOVSQ + a byte tail.
 * Both use general-purpose registers only, so neither disturbs FPU/XMM state —
 * which matters because the context switch saves only FXSAVE (x87 + XMM), not
 * YMM/ZMM. See the .asm header for why AVX paths wait on item 18. */
void *fast_memcpy(void *dst, const void *src, size_t n);

void *memcpy(void *dst, const void *src, size_t n) {
    return fast_memcpy(dst, src, n);
}

/* memmove deliberately does NOT route through fast_memcpy.
 *
 * `rep movsb` copies strictly forward, so it is correct for memmove only when
 * the destination is below the source. Sending an overlapping backward move
 * through it would corrupt the tail — and it would do so only for overlapping
 * arguments, which is exactly the case a quick test does not cover. The
 * byte loop below already handles both directions; leaving it alone is the
 * cheaper correctness guarantee. */
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y)
            return (int)x - (int)y;
        if (!x)
            break;
    }
    return 0;
}
