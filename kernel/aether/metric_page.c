/* kernel/aether/metric_page.c — F#68 sealed objective-function root (DDR-795). */
#include "metric_page.h"
#include "pmm.h"          /* pmm_alloc_page, PAGE_SIZE */
#include "vmm.h"          /* vmm_map_in, VMM_USER/VMM_NX, PTE_SW_SHARED */
#include "string.h"       /* memset */
#include "console.h"      /* kputs (panic path) */
#include "irq.h"          /* g_ticks */
#include "rtc.h"          /* DDR-812: rtc_now for last_boot_ts */
#include "sha256.h"       /* DDR-811: record integrity (first kernel caller) */
#include "errno.h"

volatile metric_page_t *metric_page;   /* NULL until metric_page_init */
uint64_t metric_page_phys;

void metric_page_init(void) {
    uint64_t frame = pmm_alloc_page();
    if (!frame) {
        kputs("\r\n*** NEXUS PANIC: metric_page_init out of memory ***\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);   /* clears KASAN poison too */
    metric_page_phys = frame;
    metric_page = (volatile metric_page_t *)(uintptr_t)frame;  /* identity = kernel RW */

    /* Stamp the header immediately. A page that is mapped but unstamped reads as
     * magic=0, which a verifier could mistake for "no objective function set"
     * rather than "the region is not initialised" — two very different states. */
    metric_page->magic   = METRIC_MAGIC;
    metric_page->version = METRIC_VERSION;
    metric_page->flags   = 0;                 /* not sealed until seal() runs */
}

void metric_page_map_user(uint64_t cr3) {
    if (!metric_page_phys)
        return;
    /* Read-only (no VMM_RW), non-executable (VMM_NX), user, SW_SHARED so the
     * teardown/fork paths share this one frame rather than freeing or copying
     * it. map_core adds PRESENT. A ring-3 store therefore takes #PF and
     * idt.c's user-fault path kills the process cleanly (ADR-021). */
    vmm_map_in(cr3, METRIC_USER_VA, metric_page_phys,
               VMM_USER | VMM_NX | PTE_SW_SHARED);
}

void metric_page_seal(const uint8_t *root, uint32_t entries) {
    if (!metric_page || !root)
        return;
    for (uint32_t i = 0; i < METRIC_ROOT_LEN; i++)
        metric_page->root[i] = root[i];
    metric_page->entries    = entries;
    metric_page->sealed_ts  = g_ticks;
    /* generation LAST, after the payload is in place: a reader that samples
     * mid-update must not see a new generation attached to an old root. */
    metric_page->generation = metric_page->generation + 1;
    metric_page->flags      = METRIC_FLAG_SEALED;
}

/* ===================== DDR-812 metric lockbox ==============================
 * The authoritative record lives in THIS frame, which ring 3 maps read-only+NX.
 * The guarantee is a page-table property, not a path check — an SFS file would
 * be writable by any CAP_FS_WRITE holder, i.e. by every CAP_SOVEREIGN process,
 * which is exactly who the lockbox guards against.
 * ========================================================================= */

/* Days since 1970-01-01 for a proleptic-Gregorian y/m/d. Howard Hinnant's
 * days_from_civil: exact for the whole range and free of the leap-year
 * special-casing that hand-rolled converters routinely get wrong by a day.
 * Static here because metric_page is the only caller; it moves to rtc.c if a
 * second one ever appears. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2u);
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2u ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint64_t rtc_to_epoch(const struct rtc_time *t) {
    int64_t days = days_from_civil((int64_t)t->year, t->month, t->day);
    return (uint64_t)(days * 86400 + t->hour * 3600 + t->minute * 60 + t->second);
}

/* Boot phases the commit is permitted in. Anything else is a bug, and the guard
 * makes it a loud one rather than a silent extra write. */
enum { LOCKBOX_PHASE_NONE = 0, LOCKBOX_PHASE_BOOT, LOCKBOX_PHASE_SHUTDOWN };
static unsigned g_lockbox_phase = LOCKBOX_PHASE_NONE;

/* Serialise the hashed fields in DECLARATION ORDER (see metric_page.h — the
 * order is binding and stated in both places). Explicit little-endian byte
 * emission rather than a struct memcpy: the struct is `volatile` and packed, and
 * relying on its in-memory image would silently couple the hash to compiler
 * layout decisions. */
static uint32_t lockbox_serialise(uint8_t *buf) {
    const volatile metric_lockbox_t *L = &metric_page->lockbox;
    uint32_t n = 0;
    for (unsigned i = 0; i < 32u; i++)  buf[n++] = L->kernel_sha[i];
    for (unsigned i = 0; i < 8u;  i++)  buf[n++] = (uint8_t)(L->boot_count     >> (i * 8));
    for (unsigned i = 0; i < 8u;  i++)  buf[n++] = (uint8_t)(L->last_boot_ts   >> (i * 8));
    buf[n++] = L->rtc_present;
    for (unsigned i = 0; i < 4u;  i++)  buf[n++] = (uint8_t)(L->gate_pass      >> (i * 8));
    for (unsigned i = 0; i < 4u;  i++)  buf[n++] = (uint8_t)(L->gate_fail      >> (i * 8));
    for (unsigned i = 0; i < 8u;  i++)  buf[n++] = (uint8_t)(L->total_uptime_s >> (i * 8));
    for (unsigned i = 0; i < METRIC_AGENTS_MAX; i++) buf[n++] = L->agent_liveness[i];
    return n;
}

/* THE only writer. `static`, so the linker makes it unnameable outside this
 * translation unit — which is the enforcement the spec's "compile-time
 * assertion" was reaching for and that C cannot express directly. */
static void lockbox_commit(unsigned phase) {
    if (!metric_page)
        return;
    if (phase != LOCKBOX_PHASE_BOOT && phase != LOCKBOX_PHASE_SHUTDOWN) {
        kputs("[lockbox] commit rejected: out-of-phase\r\n");
        return;
    }
    g_lockbox_phase = phase;

    volatile metric_lockbox_t *L = &metric_page->lockbox;

    if (phase == LOCKBOX_PHASE_BOOT) {
        /* Saturate rather than wrap: a boot counter that rolls over reads as a
         * fresh machine, which is precisely the claim an attacker would want. */
        if (L->boot_count != 0xFFFFFFFFFFFFFFFFull)
            L->boot_count = L->boot_count + 1;

        struct rtc_time t;
        rtc_now(&t);
        /* rtc_present distinguishes "booted at the epoch" from "we do not know
         * when this booted". Coercing an absent clock to 0 would turn an unknown
         * into a plausible-looking fact. */
        if (t.year >= 1970u) {
            L->rtc_present  = 1u;
            L->last_boot_ts = rtc_to_epoch(&t);
        } else {
            L->rtc_present  = 0u;
            L->last_boot_ts = 0u;
        }
    }
    L->total_uptime_s = g_ticks / 100u;     /* PIT is 100 Hz */

    uint8_t buf[32 + 8 + 8 + 1 + 4 + 4 + 8 + METRIC_AGENTS_MAX];
    uint32_t n = lockbox_serialise(buf);
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(buf, n, d);
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++)
        L->record_sha[i] = d[i];

    L->committed = 1u;                       /* LAST: a reader must never see a
                                              * committed flag over a stale hash */
}

void metric_lockbox_commit_boot(void)     { lockbox_commit(LOCKBOX_PHASE_BOOT); }
void metric_lockbox_commit_shutdown(void) { lockbox_commit(LOCKBOX_PHASE_SHUTDOWN); }

int metric_lockbox_read(metric_lockbox_t *out) {
    if (!metric_page || !out)
        return -ENOENT;
    if (!metric_page->lockbox.committed)
        return -ENOENT;                      /* never committed this boot */

    uint8_t buf[32 + 8 + 8 + 1 + 4 + 4 + 8 + METRIC_AGENTS_MAX];
    uint32_t n = lockbox_serialise(buf);
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(buf, n, d);

    /* Verify BEFORE copying anything out. A tampered record must yield no bytes
     * at all — returning "the data, but flagged" would let a careless caller use
     * it, which defeats the point. */
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++)
        if (metric_page->lockbox.record_sha[i] != d[i])
            return -ETAMPER;

    const volatile uint8_t *src = (const volatile uint8_t *)&metric_page->lockbox;
    uint8_t *dst = (uint8_t *)out;
    for (unsigned i = 0; i < sizeof(metric_lockbox_t); i++)
        dst[i] = src[i];
    return 0;
}
