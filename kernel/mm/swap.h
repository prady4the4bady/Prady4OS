/* kernel/mm/swap.h — anonymous-memory swap (DDR-1190 Amendment 1, C2.1 P1).
 *
 * A swapped-out page is a NOT-present leaf carrying PTE_SW_SWAP (bit 54) with
 * every flag bit of the page kept and a slot index in its address field
 * (A1.5). Every function here runs with the address space's mm lock held.
 * P1a: the store is RAM and swap-outs come only from SYS_SWAP_PROBE (A1.11).
 */
#pragma once
#include <stdint.h>
#include "vmm.h"        /* vmm_user_range_ok */

struct mm;

#define SWAP_SKIPPED_PINNED 1       /* swap_evict_locked: the mm holds a pin */

/* SYS_SWAP_PROBE ops (A1.12 point 9). */
#define SWAP_PROBE_EVICT    0
#define SWAP_PROBE_PIN      1
#define SWAP_PROBE_UNPIN    2
#define SWAP_PROBE_CORRUPT  3
#define SWAP_PROBE_COUNT    4
#define SWAP_PROBE_STATE    5
#define SWAP_PROBE_SNAP     6
#define SWAP_PROBE_RESTORE  7

/* Swap in every swapped-out page of [va, va+len). 0, or a negative errno; on
 * a failure the calling thread has been sent SIGKILL (A1.10 point 4). */
int  swap_prefault_locked(struct mm *mm, uint64_t va, uint64_t len, int writable);
/* One page: 1 swapped in, 0 not a swap entry, -EIO (integrity), -ENOMEM. */
int  swap_in_locked(struct mm *mm, uint64_t va);
/* Fork's pre-pass (A1.6): every swap entry of the arena comes back in. */
int  swap_in_all_locked(struct mm *mm);
/* The single eviction of P1a's reclaim: 0, SWAP_SKIPPED_PINNED, or -errno. */
int  swap_evict_locked(struct mm *mm, uint64_t va);
/* Free the slot a swap entry names (unmap, teardown). No-op for any other PTE. */
void swap_entry_release(uint64_t pte);
/* An address space is gone: drop its probe counts. */
void swap_as_gone(uint64_t cr3);
void swap_register(void);

/* A1.4: the range check every site makes, with the swap-in in front of it, so
 * a swapped-out page is brought back rather than refused as not present. The
 * caller holds mm->lock, which reclaim also takes, so the pages stay resident
 * until the caller drops it. */
static inline int swap_user_range_ok_locked(struct mm *mm, uint64_t cr3,
                                            uint64_t va, uint64_t len,
                                            int writable) {
    return swap_prefault_locked(mm, va, len, writable) == 0 &&
           vmm_user_range_ok(cr3, va, len, writable);
}
