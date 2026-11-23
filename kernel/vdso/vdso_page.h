/* kernel/vdso/vdso_page.h — kernel-updated, user-readable timekeeping page
 * (IMP-C, vDSO baseline).
 *
 * One physical frame shared by the whole system: the kernel maps it RW through
 * the low identity map and advances wall_time_ns from the PIT IRQ, while every
 * user address space maps the same frame read-only (R, NX) at VDSO_USER_VA so
 * ring 3 can read the clock without a syscall. W^X holds — no mapping is ever
 * both writable and executable (kernel view is RW-not-X, user view is R-only).
 *
 * The single 8-byte wall_time_ns is read atomically by a user `mov`, so no
 * seqlock is needed on the read side yet; `seq` is reserved for the future
 * multi-field / executable-reader (vdso_entry.asm) extension.
 */
#pragma once
#include <stdint.h>

/* DDR-1188 K7 (C1.5 P1): the TSC clock. CLOCK_MONOTONIC is
 *     ns_base + ((rdtsc - tsc_base) * mult >> shift)
 * read under seq (odd = update in progress; a reader retries until it sees the
 * same even seq before and after). The writer (vdso_time_update, from the PIT
 * tick) recomputes ns_base with the reader's own formula and then moves
 * tsc_base to now, so an update never steps the clock back. CLOCK_REALTIME is
 * realtime_base + CLOCK_MONOTONIC; realtime_base is set once from the RTC at
 * boot (K6: no clock_settime in P1). wall_time_ns keeps its offset and meaning
 * for existing readers. */
typedef struct __attribute__((packed)) {
    volatile uint64_t wall_time_ns;   /* offset 0  — nanoseconds since vdso_init (PIT) */
    volatile uint32_t seq;            /* offset 8  — seqlock generation              */
    volatile uint32_t mult;           /* offset 12 — ns per TSC cycle, << shift      */
    volatile uint64_t tsc_base;       /* offset 16 — TSC at the last update          */
    volatile uint64_t ns_base;        /* offset 24 — CLOCK_MONOTONIC at tsc_base     */
    volatile uint64_t realtime_base;  /* offset 32 — epoch ns at monotonic 0         */
    volatile uint32_t shift;          /* offset 40                                   */
    uint8_t  _pad[212];               /* offset 44 — pad to a 256-byte record        */
} vdso_data_t;
_Static_assert(sizeof(vdso_data_t) == 256, "vdso layout broken");

/* Canonical user VA of the vDSO page (PML4 slot 255 — clear of the slot-1 user
 * range, mmap arena, and stack). Read-only there; not used by copyin/copyout. */
#define VDSO_USER_VA  0x00007FFFFFF00000ull

extern volatile vdso_data_t *vdso_data;   /* kernel RW view (NULL until vdso_init) */
extern uint64_t vdso_page_phys;           /* shared frame's physical address       */

void vdso_init(void);                      /* allocate + zero the shared frame      */
void vdso_map_user(uint64_t cr3);          /* map it read-only into a user AS       */

/* DDR-1188 K7 (C1.5 P1). vdso_time_init starts the TSC clock once the TSC is
 * calibrated; until then (mult == 0) ktime_* fall back to the PIT tick count.
 * vdso_time_update runs from the PIT tick on the BSP (single writer). */
void     vdso_time_init(uint64_t tsc_hz);
void     vdso_time_update(void);
uint64_t ktime_mono_ns(void);              /* CLOCK_MONOTONIC */
uint64_t ktime_real_ns(void);              /* CLOCK_REALTIME  */
