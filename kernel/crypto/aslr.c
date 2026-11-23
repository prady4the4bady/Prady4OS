/* kernel/crypto/aslr.c — DDR-1189 L2 as written (Amendment 1 P1-4).
 * QEMU only, not verified on real hardware. */
#include "aslr.h"
#include "rng.h"
#include "console.h"

static uint64_t          g_aslr_state;   /* the TSC path's per-boot state */
static volatile uint32_t g_aslr_said;    /* bit 0: source printed; bit 1: weak printed */
static volatile uint32_t g_aslr_src;
static volatile int32_t  g_aslr_rdrand = -1;   /* -1 unknown, 0 absent, 1 present */

static int cpu_has_rdrand(void) {
    int32_t k = __atomic_load_n(&g_aslr_rdrand, __ATOMIC_RELAXED);
    if (k < 0) {
        uint32_t a = 1, b, c = 0, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        k = (int32_t)((c >> 30) & 1u);   /* CPUID.1:ECX[30] = RDRAND */
        __atomic_store_n(&g_aslr_rdrand, k, __ATOMIC_RELAXED);
    }
    return k;
}

/* CF is checked on every call; at most 10 tries per 64-bit word (L2). */
static int rdrand64(uint64_t *v) {
    for (int i = 0; i < 10; i++) {
        uint64_t x;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok) :: "cc");
        if (ok) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t mix64(uint64_t z) {          /* the splitmix64 finaliser */
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Weak by design (L2): TSC readings folded into one per-boot state. The
 * atomic add makes concurrent draws on two CPUs differ. */
static uint64_t tsc_draw(void) {
    uint64_t t = rdtsc64();
    uint64_t s = __atomic_add_fetch(&g_aslr_state, 0x9E3779B97F4A7C15ull ^ t,
                                    __ATOMIC_RELAXED);
    return mix64(s ^ ((t << 32) | (t >> 32)) ^ rdtsc64());
}

uint64_t aslr_random(void) {
    uint64_t v = 0;
    unsigned src;
    if (rng_bytes(&v, sizeof v) == 0)
        src = ASLR_SRC_RNG;
    else if (cpu_has_rdrand() && rdrand64(&v) == 0)
        src = ASLR_SRC_RDRAND;
    else {
        v = tsc_draw();
        src = ASLR_SRC_TSC;
    }
    __atomic_store_n(&g_aslr_src, src, __ATOMIC_RELAXED);
    if (!(__atomic_fetch_or(&g_aslr_said, 1u, __ATOMIC_RELAXED) & 1u)) {
        static const char *const nm[4] = { "none", "rng", "rdrand", "tsc" };
        kputs("[aslr] source=");
        kputs(nm[src & 3u]);
        kputs("\r\n");
    }
    if (src == ASLR_SRC_TSC && !(__atomic_fetch_or(&g_aslr_said, 2u, __ATOMIC_RELAXED) & 2u))
        kputs("aslr: weak entropy\r\n");
    return v;
}

unsigned aslr_source(void) {
    return __atomic_load_n(&g_aslr_src, __ATOMIC_RELAXED);
}
