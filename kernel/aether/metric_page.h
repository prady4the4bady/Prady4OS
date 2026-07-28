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

typedef struct __attribute__((packed)) {
    volatile uint32_t magic;                     /* 0  METRIC_MAGIC            */
    volatile uint32_t version;                   /* 4  layout version          */
    volatile uint64_t generation;                /* 8  bumped on every seal    */
    volatile uint64_t sealed_ts;                 /* 16 kernel ticks at seal    */
    volatile uint8_t  root[METRIC_ROOT_LEN];     /* 24 objective-function root */
    volatile uint32_t entries;                   /* 56 definitions covered     */
    volatile uint32_t flags;                     /* 60 METRIC_FLAG_*           */
    uint8_t  _pad[4032];                         /* 64 -> one 4 KiB frame      */
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
