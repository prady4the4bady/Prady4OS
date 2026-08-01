/* kernel/aether/metric_page.h — F#68 sealed objective-function root (DDR-795).
 *
 * One physical frame. The kernel writes it through the low identity map; every
 * user address space maps the SAME frame read-only + NX at METRIC_USER_VA, so
 * ring 3 can read the sealed root of the objective function but can never alter
 * it. A ring-3 store faults (#PF present/write/user) and the kernel converts it
 * into a clean process kill — the same path ADR-021's wxviol probe already
 * exercises for text.
 *
 * Why this page exists at all: F#68's Python lockbox keeps the metric content
 * AND its hash chain in one file, so anything able to write that file can
 * rewrite the entries and recompute the chain over them. The chain proves
 * internal consistency, not that the history was not replaced. Holding the root
 * somewhere ring 3 cannot write is what makes the difference detectable.
 *
 * W^X holds: the kernel view is RW-not-X, the user view is R-only + NX. No
 * mapping is ever both writable and executable.
 */
#pragma once
#include <stdint.h>

#define METRIC_MAGIC    0x4D455452u      /* "METR" */
#define METRIC_VERSION  1u
#define METRIC_ROOT_LEN 32u              /* SHA-256, raw bytes */

#define METRIC_FLAG_SEALED (1u << 0)

/* DDR-812 lockbox record. Lives inside the DDR-795 frame's padding, so the page
 * stays exactly 4 KiB and ring 3 still cannot write it — the guarantee comes
 * from the page tables (RO+NX mapping), not from a path string. An SFS file was
 * rejected: the VFS gates writes on CAP_FS_WRITE alone, which every
 * CAP_SOVEREIGN process holds, so a lockbox in SFS is writable by exactly the
 * processes it exists to guard against.
 *
 * ===================== HASH INPUT ORDER — BINDING ==========================
 * record_sha is SHA-256 over every field BELOW, in EXACTLY this declaration
 * order, as raw little-endian bytes with no padding between them:
 *
 *     kernel_sha, boot_count, last_boot_ts, rtc_present,
 *     gate_pass, gate_fail, total_uptime_s, agent_liveness
 *
 * Stated here AND in DDR-812 because an ambiguous hash input makes producer and
 * consumer diverge silently — which is the exact failure this record exists to
 * detect. A future reader must never have to reverse-engineer it.
 * ========================================================================== */
#define METRIC_AGENTS_MAX  32u

typedef struct __attribute__((packed)) {
    volatile uint8_t  kernel_sha[32];            /* SHA-256 of the running image */
    volatile uint64_t boot_count;                /* monotonic; SATURATES, never wraps */
    volatile uint64_t last_boot_ts;              /* epoch seconds from the RTC   */
    volatile uint8_t  rtc_present;               /* 0 => last_boot_ts MEANINGLESS */
    volatile uint32_t gate_pass;
    volatile uint32_t gate_fail;
    volatile uint64_t total_uptime_s;
    volatile uint8_t  agent_liveness[METRIC_AGENTS_MAX];
    volatile uint8_t  record_sha[32];            /* over the fields above, in order */
    volatile uint8_t  committed;                 /* 0 => never committed (-ENOENT) */
    volatile uint8_t  rng_source;                /* DDR-816: which entropy source  */
} metric_lockbox_t;

typedef struct __attribute__((packed)) {
    volatile uint32_t magic;                     /* 0  METRIC_MAGIC            */
    volatile uint32_t version;                   /* 4  layout version          */
    volatile uint64_t generation;                /* 8  bumped on every seal    */
    volatile uint64_t sealed_ts;                 /* 16 kernel ticks at seal    */
    volatile uint8_t  root[METRIC_ROOT_LEN];     /* 24 objective-function root */
    volatile uint32_t entries;                   /* 56 definitions covered     */
    volatile uint32_t flags;                     /* 60 METRIC_FLAG_*           */
    metric_lockbox_t  lockbox;                   /* 64 DDR-812                 */
    uint8_t  _pad[4032 - sizeof(metric_lockbox_t)];  /* -> one 4 KiB frame     */
} metric_page_t;
_Static_assert(sizeof(metric_page_t) == 4096, "metric page layout broken");

/* Canonical user VA. One page below the vDSO (VDSO_USER_VA 0x7FFFFFF00000), so
 * both live in PML4 slot 255, clear of the slot-1 user range, mmap arena and
 * stack. */
#define METRIC_USER_VA  0x00007FFFFFEFF000ull

extern volatile metric_page_t *metric_page;   /* kernel RW view (NULL pre-init) */
extern uint64_t metric_page_phys;             /* shared frame physical address  */

void metric_page_init(void);                  /* allocate, zero, stamp header   */
void metric_page_map_user(uint64_t cr3);      /* map read-only into a user AS   */
void metric_page_seal(const uint8_t *root, uint32_t entries);  /* kernel-only   */

/* DDR-812 — the ONLY two ways the lockbox is ever written.
 *
 * The spec asked for a compile-time assertion that only two call sites exist.
 * That is not expressible in C — no standard construct constrains a function's
 * callers. What IS enforced, and what these two wrappers buy:
 *   - the actual writer is `static` inside metric_page.c, so the LINKER makes it
 *     unnameable anywhere else;
 *   - these wrappers encode the phase, so a caller cannot pick one;
 *   - a runtime phase guard rejects an out-of-phase call;
 *   - smoke-metric arm D tests the property from OUTSIDE, which is the only
 *     check that tests behaviour rather than intent.
 */
void metric_lockbox_commit_boot(void);        /* after gates, before first user */
void metric_lockbox_commit_shutdown(void);    /* clean shutdown, before unmount */

/* Verify record_sha and copy out. 0 = ok, <0 = errno. Kernel-side; the syscall
 * wraps this. Never returns bytes from a record that fails verification. */
int  metric_lockbox_read(metric_lockbox_t *out);
