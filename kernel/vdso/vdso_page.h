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

typedef struct __attribute__((packed)) {
    volatile uint64_t wall_time_ns;   /* offset 0  — nanoseconds since vdso_init */
    volatile uint32_t seq;            /* offset 8  — seqlock generation (reserved) */
    uint8_t  _pad[244];               /* offset 12 — pad to a 256-byte record     */
} vdso_data_t;
_Static_assert(sizeof(vdso_data_t) == 256, "vdso layout broken");

/* Canonical user VA of the vDSO page (PML4 slot 255 — clear of the slot-1 user
 * range, mmap arena, and stack). Read-only there; not used by copyin/copyout. */
#define VDSO_USER_VA  0x00007FFFFFF00000ull

extern volatile vdso_data_t *vdso_data;   /* kernel RW view (NULL until vdso_init) */
extern uint64_t vdso_page_phys;           /* shared frame's physical address       */

void vdso_init(void);                      /* allocate + zero the shared frame      */
void vdso_map_user(uint64_t cr3);          /* map it read-only into a user AS       */
