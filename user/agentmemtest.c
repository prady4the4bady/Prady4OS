/* user/agentmemtest.c — DDR-836 agent memory gate (NSI 82/83).
 *
 * ONE ELF SPAWNED TWICE, once WITH CAP_MEMORY and once without. The instances
 * tell themselves apart from a read of a key nothing has stored: -ENOENT if the
 * capability is held, -EPERM if it is not.
 *
 * With CAP_MEMORY:
 *   1. write -> read round-trip, byte-for-byte
 *   2. read an unknown key -> -ENOENT, NOT an empty success (an agent handed a
 *      zero-length "fact" would reason from it)
 *   3. OVERWRITE an existing key -> the new value, and still only one record.
 *      A store that appends instead of replacing makes "what is X?" depend on
 *      scan order, and arms 1-2 pass on such a store.
 *
 * Without CAP_MEMORY:
 *   4. both calls -> -EPERM.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE         6
#define SYS_EXIT          4
#define SYS_MEMORY_WRITE  82
#define SYS_MEMORY_READ   83

#define EPERM   1
#define ENOENT  2

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrint(long v) {
    char b[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}

static void fail(const char *why, long rc) {
    wr("AGENTMEM FAIL: ");
    wr(why);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    char out[256];
    uint32_t outlen = 0;

    long who = nsi(SYS_MEMORY_READ, (long)"NO_SUCH_FACT", (long)out, (long)&outlen);

    if (who == -EPERM) {
        /* --- no CAP_MEMORY: arm 4 --- */
        long w = nsi(SYS_MEMORY_WRITE, (long)"SNEAK", (long)"x", 1);
        if (w != -EPERM)
            fail("write without CAP_MEMORY was not denied", w);
        wr("PRADYOS_AGENTMEM_DENY_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* Arm 2 — asserted on the call already made rather than repeating it. */
    if (who != -ENOENT)
        fail("read of an unknown key was not -ENOENT", who);

    /* Arm 1 — round trip. */
    const char *v1 = "the-first-fact";
    long w1 = nsi(SYS_MEMORY_WRITE, (long)"FACT_A", (long)v1, slen(v1));
    if (w1 != 0)
        fail("write", w1);

    long r1 = nsi(SYS_MEMORY_READ, (long)"FACT_A", (long)out, (long)&outlen);
    if (r1 != 0)
        fail("read round-trip", r1);
    if (outlen != (uint32_t)slen(v1))
        fail("round-trip length mismatch", (long)outlen);
    for (uint32_t i = 0; i < outlen; i++)
        if (out[i] != v1[i])
            fail("round-trip byte mismatch", (long)i);

    /* Arm 3 — overwrite must REPLACE, not append. A longer first value then a
     * shorter second one also catches a store that leaves stale tail bytes. */
    const char *v2 = "second";
    long w2 = nsi(SYS_MEMORY_WRITE, (long)"FACT_A", (long)v2, slen(v2));
    if (w2 != 0)
        fail("overwrite", w2);

    outlen = 0;
    long r2 = nsi(SYS_MEMORY_READ, (long)"FACT_A", (long)out, (long)&outlen);
    if (r2 != 0)
        fail("read after overwrite", r2);
    if (outlen != (uint32_t)slen(v2))
        fail("overwrite length not updated", (long)outlen);
    for (uint32_t i = 0; i < outlen; i++)
        if (out[i] != v2[i])
            fail("overwrite returned stale bytes", (long)i);

    wr("PRADYOS_AGENTMEM_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
