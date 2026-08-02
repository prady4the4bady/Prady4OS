/* user/lockboxtest.c — DDR-812 metric lockbox read/verify probe.
 *
 * Deliberately does NOT re-test arm D (ring 3 cannot write the page). That is
 * already gated by user/metrictest.c + smoke-metric, which stores to
 * METRIC_USER_VA and treats survival as failure — and whose gate pins the fault
 * address to cr2=0x00007FFFFFEFF040, i.e. offset 64, which is exactly where the
 * lockbox record now begins. The existing W^X gate therefore protects this
 * record by construction. Duplicating it here would add a second probe testing
 * the same property and a second way for them to disagree.
 *
 * What IS new, and what this probe asserts:
 *   1. SYS_METRIC_READ succeeds for a sovereign caller and returns a committed
 *      record;
 *   2. the record's SHA-256 recomputes correctly HERE, from an independently
 *      written serialiser. If the kernel's field order and this one ever
 *      diverge, the digests differ and the gate fails — which is the check
 *      working, not a nuisance.
 *
 * Asserting only "the syscall returned 0" would pass against a kernel that
 * verifies nothing, which is the stub this exists to reject.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "sha256.h"

#define SYS_WRITE        6
#define SYS_EXIT         4
#define SYS_METRIC_READ 76

#define AGENTS_MAX 32u

/* Must match metric_lockbox_t in kernel/aether/metric_page.h. */
struct lockbox {
    unsigned char      kernel_sha[32];
    unsigned long long boot_count;
    unsigned long long last_boot_ts;
    unsigned char      rtc_present;
    unsigned int       gate_pass;
    unsigned int       gate_fail;
    unsigned long long total_uptime_s;
    unsigned char      agent_liveness[AGENTS_MAX];
    unsigned char      record_sha[32];
    unsigned char      committed;
    unsigned char      rng_source;
} __attribute__((packed));

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("LOCKBOX FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Serialise the hashed fields in DECLARATION ORDER — binding, and stated in
 * metric_page.h and DDR-812. Written independently of the kernel's serialiser on
 * purpose: two implementations of one contract, so a divergence is detectable. */
static unsigned ser(const struct lockbox *L, unsigned char *b) {
    unsigned n = 0;
    for (unsigned i = 0; i < 32u; i++) b[n++] = L->kernel_sha[i];
    for (unsigned i = 0; i < 8u;  i++) b[n++] = (unsigned char)(L->boot_count     >> (i * 8));
    for (unsigned i = 0; i < 8u;  i++) b[n++] = (unsigned char)(L->last_boot_ts   >> (i * 8));
    b[n++] = L->rtc_present;
    for (unsigned i = 0; i < 4u;  i++) b[n++] = (unsigned char)(L->gate_pass      >> (i * 8));
    for (unsigned i = 0; i < 4u;  i++) b[n++] = (unsigned char)(L->gate_fail      >> (i * 8));
    for (unsigned i = 0; i < 8u;  i++) b[n++] = (unsigned char)(L->total_uptime_s >> (i * 8));
    for (unsigned i = 0; i < AGENTS_MAX; i++) b[n++] = L->agent_liveness[i];
    return n;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct lockbox rec;

    long r = nsi(SYS_METRIC_READ, (long)&rec, 0, 0);
    if (r != 0)
        fail("SYS_METRIC_READ did not return 0 for a sovereign caller");
    if (!rec.committed)
        fail("record returned with committed == 0");
    if (rec.boot_count == 0)
        fail("boot_count is zero after a boot commit");

    unsigned char buf[32 + 8 + 8 + 1 + 4 + 4 + 8 + AGENTS_MAX];
    unsigned n = ser(&rec, buf);
    unsigned char d[SHA256_DIGEST_LEN];
    sha256(buf, n, d);
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++)
        if (d[i] != rec.record_sha[i])
            fail("recomputed SHA-256 does not match record_sha");

    /* rtc_present is a fact about the machine, not a verdict — report it so a
     * reader can distinguish "booted at the epoch" from "we do not know when
     * this booted". Silently coercing an absent clock would turn an unknown into
     * a plausible-looking fact. */
    wr(rec.rtc_present ? "[lockbox] rtc_present=1\n" : "[lockbox] rtc_present=0\n");
    wr("PRADYOS_LOCKBOX_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
