/* kernel/mm/swap.h — anonymous-memory swap (DDR-1190 Amendment 1, C2.1 P1).
 *
 * P1a step 1: only the hook the user-copy pin needs. A swapped-out page is a
 * not-present PTE carrying PTE_SW_SWAP (A1.5); until such entries exist there
 * is nothing to swap in, so the hook reports success and touches nothing. The
 * swap entry, the probe store and the fault path replace this stub in the
 * following P1a steps.
 */
#pragma once
#include <stdint.h>

struct mm;

/* Swap in every swapped-out page of [va, va+len) in `mm`. The caller holds
 * mm->lock. 0, or a negative errno if a page cannot be brought back. */
static inline int swap_prefault_locked(struct mm *mm, uint64_t va, uint64_t len,
                                       int writable) {
    (void)mm; (void)va; (void)len; (void)writable;
    return 0;
}
